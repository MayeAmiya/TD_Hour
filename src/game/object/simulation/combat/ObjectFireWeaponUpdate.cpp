#include "core/container/container_types.h"
#include "game/object/simulation/combat/ObjectFireWeaponUpdate.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace engine {
namespace {

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t frames = std::max<uint32_t>(1, logicFramesPerSecond);
    const uint64_t product = static_cast<uint64_t>(milliseconds) * frames;
    return product / 1000u + (product % 1000u != 0 ? 1u : 0u);
}

[[nodiscard]] uint64_t saturatingTickAdd(uint64_t tick,
                                         uint64_t delta) noexcept {
    return delta > std::numeric_limits<uint64_t>::max() - tick
        ? std::numeric_limits<uint64_t>::max() : tick + delta;
}

[[nodiscard]] uint64_t lastRealWeaponShotTick(
    const ObjectWeaponComponent* weapons) noexcept {
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return 0;
    }
    uint64_t result = 0;
    for (const ObjectWeaponSlotRuntime& slot :
         weapons->sets[*weapons->activeWeaponSetIndex].slots) {
        result = std::max(result, slot.lastFireTick);
    }
    return result;
}

void advanceEmissionSequence(uint64_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

} // namespace

void ObjectFireWeaponUpdateSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype ||
        !templateComponent->archetype->fireWeaponUpdatePlan) {
        return;
    }
    ObjectFireWeaponUpdateComponent component;
    component.plan = templateComponent->archetype->fireWeaponUpdatePlan;
    component.instances.resize(component.plan->rules.size());
    for (size_t index = 0; index < component.plan->rules.size(); ++index) {
        const game::ObjectFireWeaponUpdateParameters& parameters =
            component.plan->rules[index];
        ObjectFireWeaponUpdateRuntime& runtime = component.instances[index];
        runtime.initialDelayCompleteTick = saturatingTickAdd(
            confirmedTick, millisecondsToTicks(
                parameters.initialDelayMilliseconds,
                logicFramesPerSecond));
        if (!parameters.weapon.empty()) {
            static_cast<void>(initializeObjectSystemWeaponRuntime(
                runtime.weapon, parameters.weapon, registry, entity, content,
                logicFramesPerSecond, confirmedTick,
                ObjectSystemWeaponInitialLoad::Immediate));
        }
    }
    ecs::emplace<ObjectFireWeaponUpdateComponent>(registry, entity,
                                                   std::move(component));
}

void ObjectFireWeaponUpdateSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectFireWeaponUpdateComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id)) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if ((health && health->effectivelyDead) ||
            isObjectDisabled(registry, entity, confirmedTick)) continue;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        if (status && status->hasAny(
                game::objectStatusBit(
                    game::ObjectStatusFlag::UnderConstruction))) {
            continue;
        }
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectFireWeaponUpdateComponent& component =
            ecs::get<ObjectFireWeaponUpdateComponent>(registry,
                                                       candidate.entity);
        if (!component.plan) continue;
        const ObjectWeaponComponent* ordinaryWeapons =
            ecs::try_get<ObjectWeaponComponent>(registry, candidate.entity);
        const uint64_t lastOrdinaryShot =
            lastRealWeaponShotTick(ordinaryWeapons);
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectFireWeaponUpdateParameters& parameters =
                component.plan->rules[index];
            ObjectFireWeaponUpdateRuntime& runtime =
                component.instances[index];
            if (!runtime.weapon.content ||
                confirmedTick < runtime.initialDelayCompleteTick) {
                continue;
            }
            const uint64_t exclusiveDelay = millisecondsToTicks(
                parameters.exclusiveWeaponDelayMilliseconds,
                logicFramesPerSecond);
            if (exclusiveDelay > 0 && confirmedTick < saturatingTickAdd(
                    lastOrdinaryShot, exclusiveDelay)) {
                continue;
            }
            const uint64_t emission = nextEmissionSequence;
            if (tryQueueObjectSystemWeaponFire(
                    runtime.weapon, registry, candidate.entity,
                    candidate.object, content, random,
                    logicFramesPerSecond, parameters.authoredOrder,
                    emission, confirmedTick, outCommands)) {
                advanceEmissionSequence(nextEmissionSequence);
            }
        }
    }
}

} // namespace engine
