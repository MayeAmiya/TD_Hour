#include "game/session/query/GameSessionAIOrderPolicy.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/object/simulation/containment/ObjectContainment.h"

#include <algorithm>

namespace engine {
namespace {

[[nodiscard]] bool policyHasObjectKind(
    const ObjectKindOfComponent* kinds, game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

ai::AISquadTargetSelection GameSessionAIOrderPolicy::squadTargetSelection(
    ObjectId subject, bool playerIssued) const noexcept
{
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(subject);
    if (!entity) return ai::AISquadTargetSelection::NoTarget;
    const ObjectAIBehaviorPolicyComponent* policy =
        ecs::try_get<ObjectAIBehaviorPolicyComponent>(m_world.m_registry, *entity);
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    const PlayerState* player = owner ? m_content.m_players.get(owner->player) : nullptr;
    if (!player) return ai::AISquadTargetSelection::NoTarget;
    if (player->controller == PlayerControllerKind::Ai && policy) {
        if (policy->attitude == ObjectAIAttitude::Sleep)
            return ai::AISquadTargetSelection::NoTarget;
        if (policy->attitude == ObjectAIAttitude::Passive)
            return ai::AISquadTargetSelection::LastDamageSource;
    }
    AiDifficulty difficulty = player->aiDifficulty;
    if (player->controller != PlayerControllerKind::Ai) {
        difficulty = m_content.m_startInfo.difficulty <= DIFFICULTY_EASY
            ? AiDifficulty::Easy
            : m_content.m_startInfo.difficulty >= DIFFICULTY_HARD
                ? AiDifficulty::Hard
                : AiDifficulty::Normal;
    }
    // RefCode applies these in this order: an explicit player command first
    // promotes the selection to HARD, then the scenario compatibility switch
    // may replace it with NORMAL for every subsequent victim choice.
    if (playerIssued) difficulty = AiDifficulty::Hard;
    if (m_presentation.m_chooseVictimAlwaysNormal)
        difficulty = AiDifficulty::Normal;
    switch (difficulty) {
    case AiDifficulty::Easy:
        return ai::AISquadTargetSelection::RandomLiveMember;
    case AiDifficulty::Hard:
        return ai::AISquadTargetSelection::FirstLiveMember;
    case AiDifficulty::None:
    case AiDifficulty::Normal:
    default:
        return ai::AISquadTargetSelection::ClosestLiveMember;
    }
}

int32_t GameSessionAIOrderPolicy::attackPriorityForTarget(
    ObjectId subject, ecs::entity target) const noexcept
{
    const std::optional<ecs::entity> subjectEntity =
        m_world.m_objects.entityFromId(subject);
    const ObjectAIBehaviorPolicyComponent* policy = subjectEntity
        ? ecs::try_get<ObjectAIBehaviorPolicyComponent>(
              m_world.m_registry, *subjectEntity)
        : nullptr;
    if (!policy || policy->attackPrioritySetId == 0 ||
        policy->attackPrioritySetId >= m_presentation.m_scriptAttackPriorityById.size())
        return 1;
    const GameSessionScriptPresentationState::ScriptAttackPrioritySetRuntime*
        set =
        m_presentation.m_scriptAttackPriorityById[policy->attackPrioritySetId];
    if (!set || set->id != policy->attackPrioritySetId) return 1;
    const auto directPriority = [this, set](ecs::entity candidate) {
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, candidate);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, candidate);
        for (auto rule = set->rules.rbegin();
             rule != set->rules.rend(); ++rule) {
            if ((rule->mutation ==
                     script::ScriptAttackPriorityMutationKind::ObjectType &&
                 type && type->name == rule->selector) ||
                (rule->mutation ==
                     script::ScriptAttackPriorityMutationKind::KindOf &&
                 policyHasObjectKind(kinds, rule->selectorKind))) {
                return rule->priority;
            }
        }
        return set->defaultPriority;
    };
    int32_t priority = directPriority(target);
    // RefCode checks the container first: an explicit zero excludes it even
    // when a high-value passenger is inside. Otherwise the most valuable
    // direct occupant can raise the container's raw priority.
    if (priority == 0) return 0;
    const ObjectContainmentComponent* containment =
        ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, target);
    if (containment) {
        for (const ObjectContainedObjectRecord& record :
             containment->objects) {
            const std::optional<ecs::entity> occupant =
                m_world.m_objects.entityFromId(record.object);
            if (occupant)
                priority = std::max(priority, directPriority(*occupant));
        }
    }
    return priority;
}

bool GameSessionAIOrderPolicy::hasExplicitAttackPrioritySet(
    ObjectId subject) const noexcept
{
    const std::optional<ecs::entity> subjectEntity =
        m_world.m_objects.entityFromId(subject);
    const ObjectAIBehaviorPolicyComponent* policy = subjectEntity
        ? ecs::try_get<ObjectAIBehaviorPolicyComponent>(
              m_world.m_registry, *subjectEntity)
        : nullptr;
    if (!policy || policy->attackPrioritySetId == 0 ||
        policy->attackPrioritySetId >=
            m_presentation.m_scriptAttackPriorityById.size()) {
        return false;
    }
    const GameSessionScriptPresentationState::ScriptAttackPrioritySetRuntime*
        set = m_presentation.m_scriptAttackPriorityById[
            policy->attackPrioritySetId];
    return set && set->id == policy->attackPrioritySetId;
}

bool GameSessionAIOrderPolicy::rejectsOrdersWhileSleeping(
    ObjectId subject) const noexcept
{
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(subject);
    const ObjectAIBehaviorPolicyComponent* policy = entity
        ? ecs::try_get<ObjectAIBehaviorPolicyComponent>(m_world.m_registry, *entity)
        : nullptr;
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
        : nullptr;
    const PlayerState* player = owner ? m_content.m_players.get(owner->player) : nullptr;
    return policy && policy->attitude == ObjectAIAttitude::Sleep &&
        player && player->controller == PlayerControllerKind::Ai;
}

std::optional<ObjectId> GameSessionAIOrderPolicy::passiveRetaliationTarget(
    ObjectId subject, bool& passive) const noexcept
{
    passive = false;
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(subject);
    const ObjectAIBehaviorPolicyComponent* policy = entity
        ? ecs::try_get<ObjectAIBehaviorPolicyComponent>(m_world.m_registry, *entity)
        : nullptr;
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
        : nullptr;
    const PlayerState* player = owner ? m_content.m_players.get(owner->player) : nullptr;
    if (!policy || policy->attitude != ObjectAIAttitude::Passive ||
        !player || player->controller != PlayerControllerKind::Ai) {
        return std::nullopt;
    }
    passive = true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
    if (!health || health->lastDamageType == game::DamageType::HEALING ||
        !health->lastDamageSource ||
        !m_world.m_objects.entityFromId(health->lastDamageSource)) {
        return std::nullopt;
    }
    return health->lastDamageSource;
}

std::optional<ObjectId> GameSessionAIOrderPolicy::attackSquadPassiveTarget(
    ObjectId subject) const noexcept
{
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(subject);
    const ObjectAIBehaviorPolicyComponent* policy = entity
        ? ecs::try_get<ObjectAIBehaviorPolicyComponent>(
              m_world.m_registry, *entity)
        : nullptr;
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
        : nullptr;
    const PlayerState* player = owner
        ? m_content.m_players.get(owner->player) : nullptr;
    if (!policy || policy->attitude != ObjectAIAttitude::Passive ||
        !player || player->controller != PlayerControllerKind::Ai) {
        return std::nullopt;
    }
    const ObjectHealthComponent* health = entity
        ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity)
        : nullptr;
    if (!health || !health->lastDamageSource ||
        !m_world.m_objects.entityFromId(health->lastDamageSource)) {
        return std::nullopt;
    }
    return health->lastDamageSource;
}

bool GameSessionAIOrderPolicy::attitudePromotesMove(
    ObjectId subject) const noexcept
{
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(subject);
    const ObjectAIBehaviorPolicyComponent* policy = entity
        ? ecs::try_get<ObjectAIBehaviorPolicyComponent>(m_world.m_registry, *entity)
        : nullptr;
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
        : nullptr;
    const PlayerState* player = owner ? m_content.m_players.get(owner->player) : nullptr;
    return policy &&
        (policy->attitude == ObjectAIAttitude::Alert ||
         policy->attitude == ObjectAIAttitude::Aggressive) &&
        player && player->controller == PlayerControllerKind::Ai;
}


} // namespace engine
