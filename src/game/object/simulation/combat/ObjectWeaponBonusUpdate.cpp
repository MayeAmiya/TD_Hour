#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] bool kindsMatch(
    const ObjectKindOfComponent* kinds,
    const game::ObjectWeaponBonusUpdateParameters& parameters) noexcept {
    return kinds && game::objectKindsMatch(
        kinds->mask, parameters.requiredAffectKinds,
        parameters.forbiddenAffectKinds);
}

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] bool isAlive(const ecs::registry& registry,
                           ecs::entity entity) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return health && !health->effectivelyDead;
}

[[nodiscard]] bool sourceCanPulse(const ecs::registry& registry,
                                  ecs::entity entity) noexcept {
    // Disabled gating is centralized at source candidate admission; this
    // helper owns only the independent terminal Body predicate.
    return isAlive(registry, entity);
}

[[nodiscard]] bool isOffMap(const ecs::registry& registry,
                            ecs::entity entity) noexcept {
    const ObjectMapStatusComponent* status =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return status && status->offMap;
}

void clearTemporaryBonus(
    ecs::registry& registry, ecs::entity entity, ObjectId target,
    ObjectId source, uint32_t authoredOrder,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectWeaponBonusUpdateEvent>& outEvents,
    bool releaseTint = true) {
    ObjectTemporaryWeaponBonusComponent* temporary =
        ecs::try_get<ObjectTemporaryWeaponBonusComponent>(registry, entity);
    if (!temporary || !temporary->current) return;
    const game::WeaponBonusCondition condition = *temporary->current;
    static_cast<void>(setObjectWeaponBonusCondition(
        registry, entity, condition, false, &content, &random,
        logicFramesPerSecond, confirmedTick));
    outEvents.push_back({
        .kind = ObjectWeaponBonusUpdateEventKind::Cleared,
        .source = source,
        .target = target,
        .condition = condition,
        .authoredOrder = authoredOrder,
        .confirmedTick = confirmedTick,
    });
    temporary->current.reset();
    temporary->removeTick = 0;
    if (releaseTint) {
        const uint64_t age = confirmedTick >= temporary->tintStartedTick
            ? confirmedTick - temporary->tintStartedTick : 0;
        temporary->tintReleaseStartFrame = static_cast<uint8_t>(
            std::min<uint64_t>(30u, age + 1u));
        temporary->tintReleaseTick = confirmedTick;
    }
}

void applyTemporaryBonus(
    ecs::registry& registry, ecs::entity entity, ObjectId target,
    ObjectId source,
    const game::ObjectWeaponBonusUpdateParameters& parameters,
    uint64_t durationTicks,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectWeaponBonusUpdateEvent>& outEvents) {
    if (parameters.bonusCondition >= game::WeaponBonusCondition::Count) return;
    ObjectTemporaryWeaponBonusComponent* temporary =
        ecs::try_get<ObjectTemporaryWeaponBonusComponent>(registry, entity);
    if (temporary && temporary->current &&
        *temporary->current != parameters.bonusCondition) {
        clearTemporaryBonus(registry, entity, target, source,
                            parameters.authoredOrder, content, random,
                            logicFramesPerSecond, confirmedTick, outEvents,
                            false);
    }
    if (!temporary) {
        temporary = &ecs::emplace<ObjectTemporaryWeaponBonusComponent>(
            registry, entity);
    }
    if (!temporary->current) {
        // A different condition replaced in the same logic transaction keeps
        // the existing FRENZY status edge; a true reactivation after release
        // starts a fresh attack envelope.
        if (temporary->tintReleaseStartFrame != 0) {
            temporary->tintStartedTick = confirmedTick;
        } else if (temporary->tintStartedTick == 0) {
            temporary->tintStartedTick = confirmedTick;
        }
        temporary->tintReleaseTick = 0;
        temporary->tintReleaseStartFrame = 0;
    }
    static_cast<void>(setObjectWeaponBonusCondition(
        registry, entity, parameters.bonusCondition, true, &content, &random,
        logicFramesPerSecond, confirmedTick));
    temporary->current = parameters.bonusCondition;
    temporary->removeTick = saturatingAdd(confirmedTick, durationTicks);
    outEvents.push_back({
        .kind = ObjectWeaponBonusUpdateEventKind::Applied,
        .source = source,
        .target = target,
        .condition = parameters.bonusCondition,
        .authoredOrder = parameters.authoredOrder,
        .confirmedTick = confirmedTick,
    });
}

} // namespace

uint64_t ObjectWeaponBonusUpdateSystem::millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

void ObjectWeaponBonusUpdateSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity, uint64_t createdAtTick) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectWeaponBonusUpdatePlan> plan =
        templateComponent && templateComponent->archetype
            ? templateComponent->archetype->weaponBonusUpdatePlan : nullptr;
    if (!plan || plan->rules.empty()) return;
    ObjectWeaponBonusUpdateComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    for (ObjectWeaponBonusUpdateRuntime& runtime : component.instances) {
        runtime.nextPulseTick = createdAtTick;
    }
    if (ObjectWeaponBonusUpdateComponent* existing =
            ecs::try_get<ObjectWeaponBonusUpdateComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectWeaponBonusUpdateComponent>(registry, entity,
                                                       std::move(component));
    }
}

void ObjectWeaponBonusUpdateSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    SimulationRandom& random, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick,
    container::Vector<ObjectWeaponBonusUpdateEvent>& outEvents) const {
    // Expire in ObjectId order before any source refreshes a status.
    container::Vector<Candidate> expirations;
    const auto temporaryView = ecs::view<const ObjectIdentityComponent,
                                         ObjectTemporaryWeaponBonusComponent>(registry);
    for (const ecs::entity entity : temporaryView) {
        const ObjectIdentityComponent& identity =
            temporaryView.template get<const ObjectIdentityComponent>(entity);
        const ObjectTemporaryWeaponBonusComponent& temporary =
            temporaryView.template get<ObjectTemporaryWeaponBonusComponent>(entity);
        if (identity.id && temporary.current &&
            confirmedTick >= temporary.removeTick) {
            expirations.push_back({.object = identity.id, .entity = entity});
        }
    }
    std::sort(expirations.begin(), expirations.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });
    for (const Candidate& candidate : expirations) {
        clearTemporaryBonus(registry, candidate.entity, candidate.object,
                            INVALID_OBJECT_ID, 0, content, random,
                            logicFramesPerSecond, confirmedTick, outEvents);
    }

    // Release-only presentation state owns no gameplay condition. Reclaim it
    // after the original 30-frame decay has completed.
    container::Vector<Candidate> released;
    const auto releasedView = ecs::view<const ObjectIdentityComponent,
                                        ObjectTemporaryWeaponBonusComponent>(registry);
    for (const ecs::entity entity : releasedView) {
        const ObjectIdentityComponent& identity =
            releasedView.template get<const ObjectIdentityComponent>(entity);
        const ObjectTemporaryWeaponBonusComponent& temporary =
            releasedView.template get<ObjectTemporaryWeaponBonusComponent>(entity);
        if (identity.id && !temporary.current &&
            temporary.tintReleaseStartFrame != 0 &&
            confirmedTick >= temporary.tintReleaseTick + 30u) {
            released.push_back({.object = identity.id, .entity = entity});
        }
    }
    std::sort(released.begin(), released.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });
    for (const Candidate& candidate : released) {
        ecs::remove<ObjectTemporaryWeaponBonusComponent>(registry,
                                                          candidate.entity);
    }

    container::Vector<Candidate> sources;
    const auto sourceView = ecs::view<const ObjectIdentityComponent,
                                      ObjectWeaponBonusUpdateComponent,
                                      const TransformComponent,
                                      const OwnerComponent>(registry);
    for (const ecs::entity entity : sourceView) {
        const ObjectIdentityComponent& identity =
            sourceView.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id) &&
            !isObjectDisabled(registry, entity, confirmedTick) &&
            sourceCanPulse(registry, entity)) {
            sources.push_back({.object = identity.id, .entity = entity});
        }
    }
    std::sort(sources.begin(), sources.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    // Build the container edge once. Contained passengers deliberately do
    // not need a Transform: RefCode reaches them through ContainInterface.
    container::TreeMap<ObjectId, container::Vector<Candidate>> containedBy;
    const auto containedView = ecs::view<const ObjectIdentityComponent,
                                         const ObjectContainedByComponent>(registry);
    for (const ecs::entity entity : containedView) {
        const ObjectIdentityComponent& identity =
            containedView.template get<const ObjectIdentityComponent>(entity);
        const ObjectContainedByComponent& contained =
            containedView.template get<const ObjectContainedByComponent>(entity);
        if (!identity.id || !contained.container ||
            lifecycle.isPendingDestroy(identity.id)) continue;
        containedBy[contained.container].push_back(
            {.object = identity.id, .entity = entity});
    }
    for (auto& [container, passengers] : containedBy) {
        static_cast<void>(container);
        std::sort(passengers.begin(), passengers.end(),
                  [](const Candidate& left, const Candidate& right) {
                      return left.object < right.object;
                  });
    }

    container::Vector<Candidate> directTargets;
    const auto targetView = ecs::view<const ObjectIdentityComponent,
                                      const TransformComponent,
                                      const OwnerComponent>(registry);
    for (const ecs::entity entity : targetView) {
        const ObjectIdentityComponent& identity =
            targetView.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            ecs::try_get<ObjectContainedByComponent>(registry, entity) ||
            !isAlive(registry, entity)) continue;
        directTargets.push_back({.object = identity.id, .entity = entity});
    }
    std::sort(directTargets.begin(), directTargets.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& source : sources) {
        ObjectWeaponBonusUpdateComponent& component =
            ecs::get<ObjectWeaponBonusUpdateComponent>(registry, source.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) continue;
        const TransformComponent& sourceTransform =
            ecs::get<const TransformComponent>(registry, source.entity);
        const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
            registry, source.entity, sourceTransform);
        const bool sourceOffMap = isOffMap(registry, source.entity);
        const OwnerComponent& sourceOwner =
            ecs::get<const OwnerComponent>(registry, source.entity);
        if (!sourceOwner.player) continue;

        for (size_t ruleIndex = 0; ruleIndex < component.plan->rules.size();
             ++ruleIndex) {
            ObjectWeaponBonusUpdateRuntime& runtime =
                component.instances[ruleIndex];
            if (runtime.lastPulseTick == confirmedTick ||
                confirmedTick < runtime.nextPulseTick) continue;
            runtime.lastPulseTick = confirmedTick;
            const game::ObjectWeaponBonusUpdateParameters& parameters =
                component.plan->rules[ruleIndex];
            runtime.nextPulseTick = saturatingAdd(
                confirmedTick,
                millisecondsToTicks(parameters.bonusDelayMilliseconds,
                                    logicFramesPerSecond));
            const uint64_t durationTicks = millisecondsToTicks(
                parameters.bonusDurationMilliseconds, logicFramesPerSecond);
            if (parameters.bonusRange < math::q32_32{}) continue;
            const math::q32_32 range = parameters.bonusRange;
            const math::q32_32 rangeSquared = range * range;

            for (const Candidate& target : directTargets) {
                const OwnerComponent& targetOwner =
                    ecs::get<const OwnerComponent>(registry, target.entity);
                if (!targetOwner.player ||
                    relationshipBetweenObjects(
                        registry, players, source.entity, target.entity) !=
                        PlayerRelationship::Allies) continue;
                if (isOffMap(registry, target.entity) != sourceOffMap) continue;
                const TransformComponent& targetTransform =
                    ecs::get<const TransformComponent>(registry, target.entity);
                const LogicFixedVec3 targetPosition =
                    readAuthoritativeObjectPosition(
                        registry, target.entity, targetTransform);
                const math::q32_32 dx = targetPosition.x - sourcePosition.x;
                const math::q32_32 dy = targetPosition.y - sourcePosition.y;
                if (dx * dx + dy * dy > rangeSquared) continue;

                const ObjectKindOfComponent* targetKinds =
                    ecs::try_get<ObjectKindOfComponent>(registry, target.entity);
                if (kindsMatch(targetKinds, parameters)) {
                    applyTemporaryBonus(
                        registry, target.entity, target.object, source.object,
                        parameters, durationTicks, content, random,
                        logicFramesPerSecond, confirmedTick, outEvents);
                }

                const auto passengerGroup = containedBy.find(target.object);
                if (passengerGroup == containedBy.end()) continue;
                for (const Candidate& passenger : passengerGroup->second) {
                    const ObjectKindOfComponent* passengerKinds =
                        ecs::try_get<ObjectKindOfComponent>(registry,
                                                            passenger.entity);
                    if (!kindsMatch(passengerKinds, parameters)) continue;
                    applyTemporaryBonus(
                        registry, passenger.entity, passenger.object,
                        source.object, parameters, durationTicks, content,
                        random, logicFramesPerSecond, confirmedTick, outEvents);
                }
            }
        }
    }
}

} // namespace engine
