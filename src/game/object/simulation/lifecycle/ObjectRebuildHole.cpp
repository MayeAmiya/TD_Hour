#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace {

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t fps) noexcept {
    if (milliseconds == 0) return 0;
    return (static_cast<uint64_t>(milliseconds) * std::max(1u, fps) + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return left > std::numeric_limits<uint64_t>::max() - right
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

} // namespace

namespace engine {

void ObjectRebuildHoleSystem::initializeObject(ecs::registry& registry,
                                               ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->rebuildHolePlan ||
        type->archetype->rebuildHolePlan->behaviors.empty()) return;
    ObjectRebuildHoleComponent component;
    component.plan = type->archetype->rebuildHolePlan;
    component.runtimes.resize(component.plan->behaviors.size());
    if (ObjectRebuildHoleComponent* existing =
            ecs::try_get<ObjectRebuildHoleComponent>(registry, entity))
        *existing = std::move(component);
    else
        ecs::emplace<ObjectRebuildHoleComponent>(registry, entity,
                                                 std::move(component));
}

void ObjectRebuildHoleSystem::onBehaviorDie(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId object, uint32_t authoredOrder,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return;
    if (const ObjectRebuildHoleComponent* hole =
            ecs::try_get<ObjectRebuildHoleComponent>(registry, *entity);
        hole && hole->plan) {
        const auto behavior = std::find_if(
            hole->plan->behaviors.begin(), hole->plan->behaviors.end(),
            [authoredOrder](const game::ObjectRebuildHoleBehaviorRule& rule) {
                return rule.authoredOrder == authoredOrder;
            });
        if (behavior != hole->plan->behaviors.end()) {
            const size_t index = static_cast<size_t>(std::distance(
                hole->plan->behaviors.begin(), behavior));
            if (index < hole->runtimes.size() &&
                hole->runtimes[index].worker) {
                static_cast<void>(lifecycle.requestDestroy(
                    hole->runtimes[index].worker,
                    ObjectDestroyReason::System, confirmedTick));
            }
        }
    }
    static_cast<void>(lifecycle.requestDestroy(
        object, ObjectDestroyReason::Combat, confirmedTick));
}

void ObjectRebuildHoleSystem::onExposeDie(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId object, ObjectId damageSource, uint32_t authoredOrder,
    uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectRebuildHoleExposeIntent>& outExpose) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, *entity);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction))) {
        return;
    }
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, *entity);
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, *entity);
    const PrimaryTeamComponent* team =
        ecs::try_get<PrimaryTeamComponent>(registry, *entity);
    const ObjectFixedTransformComponent* transform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, *entity);
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, *entity);
    if (!type || !type->archetype || !type->archetype->rebuildHolePlan ||
        !owner || owner->player == NEUTRAL_PLAYER_ID || !team || !transform ||
        !geometry) {
        return;
    }
    const auto found = std::find_if(
        type->archetype->rebuildHolePlan->exposes.begin(),
        type->archetype->rebuildHolePlan->exposes.end(),
        [&](const game::ObjectRebuildHoleExposeRule& rule) {
            return rule.authoredOrder == authoredOrder;
        });
    if (found == type->archetype->rebuildHolePlan->exposes.end() ||
        found->holeTemplate.empty()) {
        return;
    }
    ObjectRebuildExposeConsumedComponent* consumed =
        ecs::try_get<ObjectRebuildExposeConsumedComponent>(registry, *entity);
    if (consumed && std::binary_search(consumed->authoredOrders.begin(),
                                       consumed->authoredOrders.end(),
                                       authoredOrder)) {
        return;
    }
    if (!consumed) {
        consumed = &ecs::emplace<ObjectRebuildExposeConsumedComponent>(
            registry, *entity);
    }
    consumed->authoredOrders.insert(
        std::lower_bound(consumed->authoredOrders.begin(),
                         consumed->authoredOrders.end(), authoredOrder),
        authoredOrder);
    const uint64_t submissionOrdinal = nextGameplaySubmissionOrdinal++;
    if (nextGameplaySubmissionOrdinal == 0) ++nextGameplaySubmissionOrdinal;
    outExpose.push_back({
        .source = object,
        .damageSource = damageSource,
        .owner = owner->player,
        .team = team->team,
        .transform = *transform,
        .geometryShape = geometry->shape,
        .geometryIsSmall = geometry->isSmall,
        .geometryMajorRadius = geometry->majorRadiusFixed,
        .geometryMinorRadius = geometry->minorRadiusFixed,
        .geometryHeight = geometry->heightFixed,
        .geometryBoundingCircleRadius = geometry->boundingCircleRadiusFixed,
        .geometryBoundingSphereRadius = geometry->boundingSphereRadiusFixed,
        .holeTemplate = found->holeTemplate,
        .rebuildTemplate = type->archetype->name,
        .holeMaximumHealth = found->holeMaximumHealth,
        .authoredOrder = found->authoredOrder,
        .submissionOrdinal = submissionOrdinal,
        .transferAttackers = found->transferAttackers,
        .confirmedTick = confirmedTick,
    });
}

void ObjectRebuildHoleSystem::onDeathPostamble(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId object, const ObjectSimulationRules& rules,
    uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectRebuildTargetRemapIntent>& outRemaps) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, *entity);
    if (!status || !status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Reconstructing))) {
        return;
    }
    const ObjectProducerComponent* producer =
        ecs::try_get<ObjectProducerComponent>(registry, *entity);
    if (!producer || !producer->producer) return;
    const std::optional<ecs::entity> holeEntity =
        lifecycle.entityFromIdIncludingPending(producer->producer);
    ObjectRebuildHoleComponent* hole = holeEntity
        ? ecs::try_get<ObjectRebuildHoleComponent>(registry, *holeEntity)
        : nullptr;
    if (!hole || hole->runtimes.empty() || !hole->plan ||
        hole->plan->behaviors.empty()) {
        return;
    }

    const uint64_t submissionOrdinal = nextGameplaySubmissionOrdinal++;
    if (nextGameplaySubmissionOrdinal == 0) ++nextGameplaySubmissionOrdinal;
    outRemaps.push_back({
        .from = object,
        .to = producer->producer,
        .submissionOrdinal = submissionOrdinal,
        .confirmedTick = confirmedTick,
    });
    ObjectRebuildHoleRuntime& runtime = hole->runtimes.front();
    if (runtime.worker) {
        static_cast<void>(lifecycle.requestDestroy(
            runtime.worker, ObjectDestroyReason::System, confirmedTick));
    }
    runtime.worker = INVALID_OBJECT_ID;
    runtime.reconstruction = INVALID_OBJECT_ID;
    runtime.spawner = object;
    runtime.phase = ObjectRebuildHolePhase::WaitingForWorker;
    runtime.workerDueTick = saturatingAdd(
        confirmedTick, millisecondsToTicks(
            hole->plan->behaviors.front().workerRespawnDelayMilliseconds,
            rules.logicFramesPerSecond));
    ++runtime.revision;
}

void ObjectRebuildHoleSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outHealing,
    container::Vector<ObjectRebuildWorkerSpawnIntent>& outWorkers,
    container::Vector<ObjectRebuildCompletionIntent>& outCompletions,
    container::Vector<ObjectRebuildTargetRemapIntent>& outRemaps) const {
    struct Hole { ObjectId id; ecs::entity entity; };
    container::Vector<Hole> holes;
    const auto view = ecs::view<
        const ObjectIdentityComponent, ObjectRebuildHoleComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectId id = view.template get<const ObjectIdentityComponent>(entity).id;
        if (id && lifecycle.entityFromId(id)) holes.push_back({id, entity});
    }
    std::sort(holes.begin(), holes.end(),
              [](const Hole& left, const Hole& right) { return left.id < right.id; });
    for (const Hole& hole : holes) {
        ObjectRebuildHoleComponent& component =
            ecs::get<ObjectRebuildHoleComponent>(registry, hole.entity);
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, hole.entity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(registry, hole.entity);
        const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(registry, hole.entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, hole.entity);
        if (!component.plan || !owner || !team || !transform) continue;
        const size_t count = std::min(component.plan->behaviors.size(),
                                      component.runtimes.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectRebuildHoleBehaviorRule& rule =
                component.plan->behaviors[index];
            ObjectRebuildHoleRuntime& runtime = component.runtimes[index];
            if (health && !health->effectivelyDead &&
                health->currentFixed < health->maximumFixed &&
                rule.healthRegenRatioPerSecond > math::q32_32{}) {
                const math::q32_32 amount =
                    (health->maximumFixed * rule.healthRegenRatioPerSecond) /
                    math::q32_32{static_cast<int32_t>(
                        std::max(1u, rules.logicFramesPerSecond))};
                outHealing.push_back({
                    .target = hole.id,
                    .source = hole.id,
                    .sourceSequence = rule.authoredOrder,
                    .amount = amount,
                    .damageType = game::DamageType::HEALING,
                    .confirmedTick = confirmedTick,
                });
            }
            if (runtime.worker && !lifecycle.entityFromId(runtime.worker)) {
                runtime.worker = INVALID_OBJECT_ID;
                runtime.phase = ObjectRebuildHolePhase::WaitingForWorker;
                runtime.workerDueTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(
                        rule.workerRespawnDelayMilliseconds,
                        rules.logicFramesPerSecond));
                ++runtime.revision;
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, hole.entity,
                    {.clearMask = game::objectStatusBit(
                         game::ObjectStatusFlag::Masked),
                     .confirmedTick = confirmedTick}));
            }
            if (runtime.reconstruction &&
                !lifecycle.entityFromId(runtime.reconstruction)) {
                outRemaps.push_back({
                    .from = runtime.reconstruction,
                    .to = hole.id,
                    .confirmedTick = confirmedTick,
                });
                if (runtime.worker) static_cast<void>(lifecycle.requestDestroy(
                    runtime.worker, ObjectDestroyReason::System,
                    confirmedTick));
                runtime.worker = INVALID_OBJECT_ID;
                runtime.reconstruction = INVALID_OBJECT_ID;
                runtime.phase = ObjectRebuildHolePhase::WaitingForWorker;
                runtime.workerDueTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(
                        rule.workerRespawnDelayMilliseconds,
                        rules.logicFramesPerSecond));
                ++runtime.revision;
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, hole.entity,
                    {.clearMask = game::objectStatusBit(
                         game::ObjectStatusFlag::Masked),
                     .confirmedTick = confirmedTick}));
            }
            if (runtime.phase == ObjectRebuildHolePhase::WaitingForWorker &&
                confirmedTick >= runtime.workerDueTick &&
                !rule.workerTemplate.empty() && !runtime.rebuildTemplate.empty()) {
                outWorkers.push_back({
                    .hole = hole.id,
                    .reconstruction = runtime.reconstruction,
                    .owner = owner->player,
                    .team = team->team,
                    .transform = *transform,
                    .workerTemplate = rule.workerTemplate,
                    .rebuildTemplate = runtime.rebuildTemplate,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
                runtime.phase = ObjectRebuildHolePhase::AwaitingWorkerSpawn;
                ++runtime.revision;
            }
            if (runtime.phase == ObjectRebuildHolePhase::Reconstructing &&
                runtime.reconstruction) {
                const std::optional<ecs::entity> reconstruction =
                    lifecycle.entityFromId(runtime.reconstruction);
                const ObjectStatusComponent* status = reconstruction
                    ? ecs::try_get<ObjectStatusComponent>(registry, *reconstruction)
                    : nullptr;
                if (reconstruction && (!status || !status->hasAny(
                        game::objectStatusBit(
                            game::ObjectStatusFlag::UnderConstruction)))) {
                    outCompletions.push_back({
                        .hole = hole.id,
                        .worker = runtime.worker,
                        .reconstruction = runtime.reconstruction,
                        .confirmedTick = confirmedTick,
                    });
                    runtime.phase = ObjectRebuildHolePhase::Dormant;
                    ++runtime.revision;
                }
            }
        }
    }
}

bool ObjectRebuildHoleSystem::startHole(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId hole,
    container::StringView rebuildTemplate, ObjectId spawner,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(hole);
    ObjectRebuildHoleComponent* component = entity
        ? ecs::try_get<ObjectRebuildHoleComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan || component->runtimes.empty()) return false;
    ObjectRebuildHoleRuntime& runtime = component->runtimes.front();
    if (runtime.worker) static_cast<void>(lifecycle.requestDestroy(
        runtime.worker, ObjectDestroyReason::System, confirmedTick));
    runtime.worker = INVALID_OBJECT_ID;
    runtime.reconstruction = INVALID_OBJECT_ID;
    runtime.spawner = spawner;
    runtime.rebuildTemplate = rebuildTemplate;
    runtime.workerDueTick = saturatingAdd(
        confirmedTick, millisecondsToTicks(
            component->plan->behaviors.front().workerRespawnDelayMilliseconds,
            rules.logicFramesPerSecond));
    runtime.phase = ObjectRebuildHolePhase::WaitingForWorker;
    ++runtime.revision;
    return true;
}

bool ObjectRebuildHoleSystem::acknowledgeWorker(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId hole,
    ObjectId worker, ObjectId reconstruction, uint64_t) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(hole);
    ObjectRebuildHoleComponent* component = entity
        ? ecs::try_get<ObjectRebuildHoleComponent>(registry, *entity) : nullptr;
    if (!component || component->runtimes.empty() ||
        !lifecycle.entityFromId(worker) || !lifecycle.entityFromId(reconstruction))
        return false;
    ObjectRebuildHoleRuntime& runtime = component->runtimes.front();
    runtime.worker = worker;
    runtime.reconstruction = reconstruction;
    runtime.phase = ObjectRebuildHolePhase::Reconstructing;
    ++runtime.revision;
    return true;
}

bool ObjectRebuildHoleSystem::rejectWorker(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId hole,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(hole);
    ObjectRebuildHoleComponent* component = entity
        ? ecs::try_get<ObjectRebuildHoleComponent>(registry, *entity) : nullptr;
    if (!component || !component->plan || component->runtimes.empty()) return false;
    ObjectRebuildHoleRuntime& runtime = component->runtimes.front();
    runtime.phase = ObjectRebuildHolePhase::WaitingForWorker;
    runtime.workerDueTick = saturatingAdd(
        confirmedTick, millisecondsToTicks(
            component->plan->behaviors.front().workerRespawnDelayMilliseconds,
            rules.logicFramesPerSecond));
    ++runtime.revision;
    static_cast<void>(ObjectStatusSystem::apply(
        registry, *entity,
        {.clearMask = game::objectStatusBit(game::ObjectStatusFlag::Masked),
         .confirmedTick = confirmedTick}));
    return true;
}

} // namespace engine
