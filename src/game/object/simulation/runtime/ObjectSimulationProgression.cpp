#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"

#include <utility>

namespace engine {

void ObjectSimulation::finalizeExperienceMutation(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectExperienceMutation& mutation, ObjectId source,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context,
    bool provideFeedback) {
    if (!mutation.accepted)
        return;
    if (mutation.pointsChanged) {
        object_simulation_detail::state(*this).m_experienceEvents.push_back({
            .kind = ObjectExperienceEventKind::PointsChanged,
            .object = mutation.receiver,
            .source = source,
            .previousPoints = mutation.previousPoints,
            .currentPoints = mutation.currentPoints,
            .appliedPoints = mutation.appliedPoints,
            .previousLevel = mutation.previousLevel,
            .currentLevel = mutation.currentLevel,
            .confirmedTick = confirmedTick,
            .provideFeedback = provideFeedback,
        });
    }
    if (!mutation.levelChanged)
        return;
    object_simulation_detail::state(*this).m_experienceEvents.push_back({
        .kind = ObjectExperienceEventKind::VeterancyChanged,
        .object = mutation.receiver,
        .source = source,
        .previousPoints = mutation.previousPoints,
        .currentPoints = mutation.currentPoints,
        .appliedPoints = mutation.appliedPoints,
        .previousLevel = mutation.previousLevel,
        .currentLevel = mutation.currentLevel,
        .confirmedTick = confirmedTick,
        .provideFeedback = provideFeedback,
    });

    const std::optional<ecs::entity> entity = lifecycle.entityFromId(mutation.receiver);
    if (!entity)
        return;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *entity);
    // The modern RefCode guard changes tracker level but suppresses bonuses,
    // local upgrades and feedback for an effectively-dead Object.
    if (health && health->effectivelyDead)
        return;

    UpgradeMask resolvedOwnerUpgrades = ownerCompletedUpgrades;
    if (context.players) {
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
        const PlayerState* player = owner ? context.players->get(owner->player) : nullptr;
        if (player) resolvedOwnerUpgrades = player->upgrades.completed;
    }
    // Object::onVeterancyLevelChanged first rechecks every UpgradeMux against
    // the already-owned player/object bits, then giveUpgrade(newLevel) adds
    // the synthetic veterancy bit and rechecks a second time. The two passes
    // are observably distinct when an earlier rule removes a trigger or a
    // dormant rule conflicts with the incoming level.
    object_simulation_detail::state(*this).m_upgrades.reevaluateObjectUpgrades(
        registry, lifecycle, mutation.receiver, resolvedOwnerUpgrades,
        object_simulation_detail::state(*this).m_rules, confirmedTick, context);
    const UpgradeContentId veterancyUpgrade = context.content
        ? context.content->veterancyUpgradeId(mutation.currentLevel)
        : INVALID_UPGRADE_CONTENT_ID;
    if (veterancyUpgrade) {
        static_cast<void>(object_simulation_detail::state(*this).m_upgrades.completeObjectUpgrade(
            registry, lifecycle, mutation.receiver,
            veterancyUpgrade, resolvedOwnerUpgrades,
            object_simulation_detail::state(*this).m_rules, confirmedTick, context));
    }
    object_simulation_detail::state(*this).m_experience.projectVeterancy(
        registry, *entity, mutation.previousLevel, mutation.currentLevel,
        object_simulation_detail::state(*this).m_rules, context.content, context.random, confirmedTick);
}

ObjectExperienceMutation ObjectSimulationProgressionDomain::setObjectVeterancyLevel(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
    game::ObjectVeterancyLevel level,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
    const ObjectExperienceMutation mutation = object_simulation_detail::state(*this).m_experience.setLevel(
        registry, lifecycle, object, level, confirmedTick);
    static_cast<ObjectSimulation&>(*this).finalizeExperienceMutation(
        registry, lifecycle, mutation, INVALID_OBJECT_ID,
        ownerCompletedUpgrades, confirmedTick, context);
    return mutation;
}

ObjectExperienceMutation ObjectSimulationProgressionDomain::addObjectExperience(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
    int32_t points, bool canScaleForBonus, ObjectId source,
    const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
    const ObjectExperienceMutation mutation = object_simulation_detail::state(*this).m_experience.addPoints(
        registry, lifecycle, object, points, canScaleForBonus, confirmedTick);
    static_cast<ObjectSimulation&>(*this).finalizeExperienceMutation(
        registry, lifecycle, mutation, source,
        ownerCompletedUpgrades, confirmedTick, context);
    return mutation;
}

bool ObjectSimulationProgressionDomain::completeObjectUpgrade(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
    UpgradeContentId upgrade, const UpgradeMask& ownerCompletedUpgrades,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) const {
    const bool completed = object_simulation_detail::state(*this).m_upgrades.completeObjectUpgrade(
        registry, lifecycle, object, upgrade,
        ownerCompletedUpgrades, object_simulation_detail::state(*this).m_rules, confirmedTick, context);
    object_simulation_detail::state(*this).m_countermeasures.reevaluateObject(
        registry, lifecycle, object, ownerCompletedUpgrades, confirmedTick,
        context.content ? context.content->upgradeCatalog() : nullptr);
    return completed;
}

void ObjectSimulationProgressionDomain::onPlayerUpgradeCompleted(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectOwnershipIndex& ownership, PlayerId player,
    const UpgradeMask& completedUpgrades,
    uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    // All currently migrated UpgradeMux consumers (typed Upgrade modules,
    // AutoHeal and FXListDie) share this one author-order transaction.  In
    // particular, RemovalsUpgrades must reset every relevant mux before a
    // later authored implementation runs; independent fan-out passes cannot
    // reproduce that source ordering.
    const UpgradeCatalog* catalog =
        context.content ? context.content->upgradeCatalog() : nullptr;
    object_simulation_detail::state(*this).m_upgrades.onPlayerUpgradeCompleted(
        registry, lifecycle, ownership, player, completedUpgrades,
        object_simulation_detail::state(*this).m_rules, confirmedTick, context);
    for (const ObjectId object : ownership.objects(player)) {
        object_simulation_detail::state(*this).m_countermeasures.reevaluateObject(
            registry, lifecycle, object, completedUpgrades, confirmedTick,
            catalog);
    }
}

} // namespace engine
