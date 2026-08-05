#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/economy/ObjectBuilder.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace engine {

void ObjectSimulation::updateConstructionRepairPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_economy.updateRepairDocks(
        registry, lifecycle, simulationState.m_rules, confirmedTick, damage);
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
    damage.clear();
    container::Vector<ObjectConstructionCompletionIntent>
        completedConstructions;
    const size_t scaffoldBegin = object_simulation_detail::state(*this)
        .m_bridgeRepairScaffoldIntents.size();
    simulationState.m_builder.update(registry, lifecycle, context.players,
                     context.mapVisibility, context.content,
                     context.navigation,
                     simulationState.m_rules, confirmedTick,
                     damage, completedConstructions,
                     object_simulation_detail::state(*this).m_bridgeRepairScaffoldIntents);
    // DozerAIUpdate advances construction with ActiveBody::internalChangeHealth,
    // not attemptHealing.  The logical BodyDamageType still follows HP, but
    // this direct mutation publishes no Healing/transition callbacks and does
    // not select damaged art while UNDER_CONSTRUCTION.  Ordinary repairs stay
    // on the full Healing transaction path below.
    const auto applyConstructionProgress =
        [this, &registry, &lifecycle, confirmedTick](
            const ObjectDamageRequest& request) {
        if (request.damageType != game::DamageType::HEALING ||
            request.amount <= ObjectHealthComponent::Scalar{}) {
            return false;
        }
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(request.target);
        if (!entity) return false;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, *entity);
        const ObjectConstructionSiteComponent* site =
            ecs::try_get<ObjectConstructionSiteComponent>(registry, *entity);
        if (!status || !site || site->lastProgressTick != confirmedTick ||
            !status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction))) {
            return false;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *entity);
        if (!health) return true;
        const ObjectHealthComponent::Scalar remaining =
            ObjectHealthComponent::Scalar::max(
                ObjectHealthComponent::Scalar{},
                health->maximumFixed - health->currentFixed);
        const ObjectHealthComponent::Scalar desired =
            request.amount >= remaining
                ? health->maximumFixed
                : health->currentFixed + request.amount;
        static_cast<void>(applyBodyHealthProjection(
            registry, lifecycle,
            {.object = request.target,
             .source = request.source,
             .desiredHealth = desired,
             .authoredOrder = request.sourceSequence,
             .confirmedTick = confirmedTick}));
        return true;
    };

    // RefCode finishes the builder's final direct Body pulse before clearing
    // UNDER_CONSTRUCTION for that object. Preserve that per-object causal
    // order while unrelated repair Healing requests retain admission order.
    container::Vector<size_t> lastHealingIndex(
        completedConstructions.size(), std::numeric_limits<size_t>::max());
    for (size_t index = 0; index < damage.size(); ++index) {
        if (damage[index].damageType != game::DamageType::HEALING) continue;
        const auto completed = std::lower_bound(
            completedConstructions.begin(), completedConstructions.end(),
            damage[index].target,
            [](const ObjectConstructionCompletionIntent& left,
               ObjectId object) { return left.object < object; });
        if (completed != completedConstructions.end() &&
            completed->object == damage[index].target) {
            lastHealingIndex[static_cast<size_t>(
                completed - completedConstructions.begin())] = index;
        }
    }
    const auto publishCompletion =
        [this, confirmedTick](ObjectConstructionCompletionIntent intent) {
            intent.submissionOrdinal = reserveGameplaySubmissionOrdinal();
            intent.confirmedTick = confirmedTick;
            object_simulation_detail::state(*this)
                .m_completedObjectConstructions.push_back(
                    std::move(intent));
    };
    for (size_t index = scaffoldBegin;
         index < object_simulation_detail::state(*this)
                     .m_bridgeRepairScaffoldIntents.size(); ++index) {
        object_simulation_detail::state(*this)
            .m_bridgeRepairScaffoldIntents[index].submissionOrdinal =
                reserveGameplaySubmissionOrdinal();
    }
    for (size_t index = 0; index < damage.size(); ++index) {
        if (!applyConstructionProgress(damage[index]))
            queueDamage(std::move(damage[index]));
        for (size_t completed = 0;
             completed < completedConstructions.size(); ++completed) {
            if (lastHealingIndex[completed] == index)
                publishCompletion(completedConstructions[completed]);
        }
    }
    for (size_t completed = 0;
         completed < completedConstructions.size(); ++completed) {
        if (lastHealingIndex[completed] == std::numeric_limits<size_t>::max())
            publishCompletion(completedConstructions[completed]);
    }
}

void ObjectSimulation::updateRebuildRecoveryPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    const size_t exposeBegin = object_simulation_detail::state(*this)
        .m_rebuildExposeIntents.size();
    const size_t workerBegin = object_simulation_detail::state(*this)
        .m_rebuildWorkerIntents.size();
    const size_t completionBegin = object_simulation_detail::state(*this)
        .m_rebuildCompletionIntents.size();
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_rebuildHole.update(
        registry, lifecycle, simulationState.m_rules, confirmedTick, damage,
        object_simulation_detail::state(*this).m_rebuildWorkerIntents, object_simulation_detail::state(*this).m_rebuildCompletionIntents,
        object_simulation_detail::state(*this).m_rebuildTargetRemapIntents);
    const auto stamp = [this](auto& intents, size_t begin) {
        for (size_t index = begin; index < intents.size(); ++index) {
            intents[index].submissionOrdinal =
                reserveGameplaySubmissionOrdinal();
        }
    };
    stamp(object_simulation_detail::state(*this).m_rebuildExposeIntents,
          exposeBegin);
    stamp(object_simulation_detail::state(*this).m_rebuildWorkerIntents,
          workerBegin);
    stamp(object_simulation_detail::state(*this).m_rebuildCompletionIntents,
          completionBegin);
    for (ObjectDamageRequest& request : damage)
        queueDamage(std::move(request));
}

void ObjectSimulation::updateWarehouseRecoveryPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {

    // The warehouse-specific healer is intentionally independent from
    // AutoHealBehavior: it is always armed by damage, uses its own suppression
    // and pulse delay, and also owns the ReallyDamaged dock-cripple fact.
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_supplyWarehouseCrippling.update(
        registry, lifecycle, confirmedTick, damage);
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
}

} // namespace engine
