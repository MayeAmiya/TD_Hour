#include "game/session/transaction/GameSessionObjectOwnershipTransactions.h"

#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

#include "game/session/object/GameSessionObjectLifecycleDetail.h"
#include "game/session/state/GameSessionDomainState.h"

#include "game/base/GameSettings.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/economy/ObjectEnergy.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/player/MatchSetup.h"
#include "game/player/PlayerRegistry.h"
#include "game/scenario/runtime/ScenarioDefinition.h"
#include "game/data/base/DifficultySimulationRules.h"

#include <algorithm>
#include <optional>

namespace engine {
using namespace object_lifecycle_detail;

GameSessionObjectOwnershipTransactions::GameSessionObjectOwnershipTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecyclePublisher,
    GameSessionGameplayPublicationPort* publication) noexcept
    : m_content(content),
      m_world(world),
      m_ai(ai),
      m_presentation(presentation),
      m_lifecyclePublisher(lifecyclePublisher),
      m_publication(publication) {}

void GameSessionObjectOwnershipTransactions::refreshDerivedAggregates(
    uint64_t confirmedTick) {
    m_world.m_objectEnergy.update(
        m_world.m_registry, m_content.m_players, confirmedTick);
    m_world.m_objectSimulation.updateRadarProviders(
        m_world.m_registry, m_world.m_objects, m_content.m_players,
        confirmedTick);
}

void GameSessionObjectOwnershipTransactions::publishLifecycleAfterOwnerChange() {
    if (!m_lifecyclePublisher) return;
    // Session still owns the singular Created/Destroy/AI-membership cascade.
    // Ownership transfer only requests a barrier flush after owner mutation.
    static_cast<void>(m_lifecyclePublisher.consumeObjectLifecycleEvents());
}

bool GameSessionObjectOwnershipTransactions::applyDifficultyBonusPolicy(
    ObjectId object, bool receiving, uint64_t confirmedTick) {
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromIdIncludingPending(object);
    if (!entity) return false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    const PlayerState* player =
        owner ? m_content.m_players.get(owner->player) : nullptr;
    if (!player) return false;

    const bool singlePlayer =
        m_content.m_startInfo.mode == GameMode::SinglePlayer ||
        m_content.m_startInfo.mode == GameMode::Challenge ||
        (m_content.m_startInfo.mode == GameMode::Replay &&
         m_content.m_resolvedMatchSetup &&
         (m_content.m_resolvedMatchSetup->mode == GameMode::SinglePlayer ||
          m_content.m_resolvedMatchSetup->mode == GameMode::Challenge));
    const bool human = player->controller == PlayerControllerKind::Human;
    const bool computer = player->controller == PlayerControllerKind::Ai ||
                          player->controller == PlayerControllerKind::Neutral;
    size_t difficulty = static_cast<size_t>(DIFFICULTY_NORMAL);
    if (human) {
        difficulty = static_cast<size_t>(std::clamp<int>(
            m_content.m_startInfo.difficulty,
            static_cast<int>(DIFFICULTY_EASY),
            static_cast<int>(DIFFICULTY_HARD)));
    } else if (computer) {
        switch (player->aiDifficulty) {
        case AiDifficulty::Easy: difficulty = DIFFICULTY_EASY; break;
        case AiDifficulty::Hard: difficulty = DIFFICULTY_HARD; break;
        case AiDifficulty::None:
        case AiDifficulty::Normal:
        default: difficulty = DIFFICULTY_NORMAL; break;
        }
    }

    using Scalar = math::q32_32;
    const Scalar identity{int32_t{1}};
    const bool applies = receiving && singlePlayer && (human || computer);
    const Scalar desiredHealth = applies
        ? m_content.m_objectSimulationRules.difficulty.healthMultiplier(
              human ? DifficultySimulationRules::kHumanIndex
                    : DifficultySimulationRules::kComputerIndex,
              difficulty)
        : identity;
    const game::WeaponBonusCondition desiredWeapon = !applies
        ? game::WeaponBonusCondition::Count
        : human
            ? static_cast<game::WeaponBonusCondition>(
                  static_cast<uint8_t>(
                      game::WeaponBonusCondition::SoloHumanEasy) +
                  static_cast<uint8_t>(difficulty))
            : static_cast<game::WeaponBonusCondition>(
                  static_cast<uint8_t>(
                      game::WeaponBonusCondition::SoloAiEasy) +
                  static_cast<uint8_t>(difficulty));

    ObjectDifficultyBonusComponent* ledger =
        ecs::try_get<ObjectDifficultyBonusComponent>(m_world.m_registry, *entity);
    if (!ledger) {
        ledger = &ecs::emplace<ObjectDifficultyBonusComponent>(
            m_world.m_registry, *entity);
    }
    if (ledger->receiving == receiving &&
        ledger->appliedHealthMultiplier == desiredHealth &&
        ledger->appliedWeaponCondition == desiredWeapon) {
        return false;
    }

    if (ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity)) {
        if (ledger->appliedHealthMultiplier > Scalar{} &&
            ledger->appliedHealthMultiplier != identity) {
            health->currentFixed /= ledger->appliedHealthMultiplier;
            health->previousFixed /= ledger->appliedHealthMultiplier;
            health->maximumFixed /= ledger->appliedHealthMultiplier;
            health->initialFixed /= ledger->appliedHealthMultiplier;
        }
        if (desiredHealth != identity) {
            health->currentFixed *= desiredHealth;
            health->previousFixed *= desiredHealth;
            health->maximumFixed *= desiredHealth;
            health->initialFixed *= desiredHealth;
        }
    }
    if (ObjectWeaponBonusComponent* weapon =
            ecs::try_get<ObjectWeaponBonusComponent>(m_world.m_registry, *entity)) {
        if (ledger->appliedWeaponCondition <
            game::WeaponBonusCondition::Count) {
            weapon->conditions &= ~game::weaponBonusConditionBit(
                ledger->appliedWeaponCondition);
        }
        if (desiredWeapon < game::WeaponBonusCondition::Count) {
            weapon->conditions |=
                game::weaponBonusConditionBit(desiredWeapon);
        }
        ++weapon->revision;
        if (weapon->revision == 0) ++weapon->revision;
        weapon->lastChangedTick = confirmedTick;
    }
    ledger->receiving = receiving;
    ledger->appliedHealthMultiplier = desiredHealth;
    ledger->appliedWeaponCondition = desiredWeapon;
    ++ledger->revision;
    if (ledger->revision == 0) ++ledger->revision;
    markObjectDirty(
        m_world.m_registry, *entity,
        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
            objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    return true;
}

bool GameSessionObjectOwnershipTransactions::transferObjectToTeam(
    ObjectId id, ObjectTeamId team, uint64_t confirmedTick) {
    if (!m_content.m_active || !id || !team) return false;
    const std::optional<PlayerId> teamOwner = m_world.m_objectTeams.teamOwner(team);
    const bool pendingDestroy = m_world.m_objects.isPendingDestroy(id);
    const std::optional<ecs::entity> entity = pendingDestroy
        ? m_world.m_objects.entityFromIdIncludingPending(id)
        : m_world.m_objects.entityFromId(id);
    if (!teamOwner || !m_content.m_players.get(*teamOwner) || !entity) return false;

    OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    PrimaryTeamComponent* primaryTeam =
        ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *entity);
    if (!owner) return false;
    const PlayerId previousObjectOwner = owner->player;
    const bool ownerChanged = owner->player != *teamOwner;
    const bool teamChanged = !primaryTeam || primaryTeam->team != team;
    if (!ownerChanged && !teamChanged) return false;

    ObjectAIAttitude inheritedAttitude = ObjectAIAttitude::Normal;
    if (m_presentation.m_scenarioDefinition) {
        for (const scenario::ScriptTeamDefinition& definition :
             m_presentation.m_scenarioDefinition->scriptTeams()) {
            const container::Span<const ObjectTeamId> instances =
                m_world.m_objectTeams.scenarioTeamInstances(definition.id);
            if (std::find(instances.begin(), instances.end(), team) ==
                instances.end()) {
                continue;
            }
            inheritedAttitude = static_cast<ObjectAIAttitude>(
                std::clamp(definition.plan.initialAttitude, -2, 2));
            break;
        }
    }

    // Freeze structural Contain dependents before changing the host.  Helix
    // and Overlord onCapture transfer their portable add-on to the new team;
    // doing it here covers every setTeam-style ingress, not only Defector.
    const container::Vector<ObjectId> containmentCaptureDependents =
        ownerChanged && !pendingDestroy
        ? m_world.m_objectSimulation.containmentCaptureDependents(
              m_world.m_registry, m_world.m_objects, id)
        : container::Vector<ObjectId>{};

    // assignObject removes any old membership before adding the new one. Both
    // component updates happen before lifecycle events are published, making
    // every external observer see only a consistent Player + primary Team.
    if (!m_world.m_objectTeams.assignObject(team, id)) return false;
    if (!primaryTeam) {
        primaryTeam = &ecs::emplace<PrimaryTeamComponent>(
            m_world.m_registry, *entity,
            PrimaryTeamComponent{.team = team});
    } else {
        primaryTeam->team = team;
    }
    // Object::setTeam resets AI attitude from the destination Team template
    // and reapplies that prototype's attack-priority name. Team-template
    // attitude is frozen from the destination Team prototype; a script-set
    // per-object mood never leaks across a transfer.
    ObjectAIBehaviorPolicyComponent* aiPolicy =
        ecs::try_get<ObjectAIBehaviorPolicyComponent>(
            m_world.m_registry, *entity);
    const auto inheritedPriority = m_world.m_objectTeams.attackPrioritySet(team);
    uint32_t inheritedSetId = 0;
    if (inheritedPriority) {
        const auto inheritedSet = m_presentation.m_scriptAttackPrioritySets.find(
            container::String{*inheritedPriority});
        if (inheritedSet != m_presentation.m_scriptAttackPrioritySets.end())
            inheritedSetId = inheritedSet->second.id;
    }
    if (!aiPolicy && (inheritedPriority || ownerChanged || teamChanged)) {
        aiPolicy = &ecs::emplace<ObjectAIBehaviorPolicyComponent>(
            m_world.m_registry, *entity,
            ObjectAIBehaviorPolicyComponent{
                .attackPrioritySetId = inheritedSetId,
                .attitude = inheritedAttitude,
            });
    } else if (aiPolicy) {
        if (aiPolicy->attackPrioritySetId != inheritedSetId ||
            aiPolicy->attitude != inheritedAttitude) {
            aiPolicy->attackPrioritySetId = inheritedSetId;
            aiPolicy->attitude = inheritedAttitude;
            ++aiPolicy->revision;
            if (aiPolicy->revision == 0) ++aiPolicy->revision;
        }
    }
    if (const auto relationshipPolicy =
            m_world.m_objectTeams.relationshipPolicy(team)) {
        if (ObjectRelationshipOverrideComponent* current =
                ecs::try_get<ObjectRelationshipOverrideComponent>(
                    m_world.m_registry, *entity)) {
            current->policy = relationshipPolicy;
        } else {
            ecs::emplace<ObjectRelationshipOverrideComponent>(
                m_world.m_registry, *entity,
                ObjectRelationshipOverrideComponent{
                    .policy = relationshipPolicy,
                });
        }
    } else {
        ecs::remove<ObjectRelationshipOverrideComponent>(
            m_world.m_registry, *entity);
    }
    if (ownerChanged && !pendingDestroy)
    {
        // Object::onCapture idles captured units unless the old and new
        // controllers are allies. Clear both the ECS queue and detached SoA
        // admission so an old attack/build/navigation order cannot continue
        // under the new controller.
        if (m_content.m_players.relationship(
                previousObjectOwner, *teamOwner) !=
            PlayerRelationship::Allies) {
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(
                    m_world.m_registry, *entity);
            if (!queue && m_ai.m_objectAI.find(id)) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    m_world.m_registry, *entity);
            }
            if (queue) {
                queue->orders.clear();
                ++queue->revision;
                ++queue->externalRevision;
                if (queue->externalRevision == 0)
                    ++queue->externalRevision;
                if (m_ai.m_objectAI.find(id)) {
                    static_cast<void>(m_ai.m_objectAI.
                        synchronizeOrderExternalRevision(
                            id, queue->externalRevision));
                    static_cast<void>(m_ai.m_objectAI.
                        clearSubjectTransients(id));
                }
            }
        }
        // Match Object::defect: production belongs to the old controller and
        // is cancelled before the factory becomes usable by its new owner.
        // This also releases any global PLAYER-upgrade reservation made by
        // that factory's queue.
        static_cast<void>(
            m_world.m_objectProduction.cancelAndRefundForOwnershipTransfer(
                m_world.m_registry, m_world.m_objects, m_content.m_players, id));
    }
    // Re-read the pending-destroy state instead of reusing the value latched at
    // the top of this function: the order-queue reset and
    // cancelAndRefundForOwnershipTransfer above can retire the object in
    // between.  With the stale value, changeOwner refused the transfer and this
    // returned false after the team registry, PrimaryTeamComponent, AI policy
    // and production refunds had already been applied — leaving the team index
    // on the new team while OwnerComponent still named the old player.
    const bool pendingDestroyNow = m_world.m_objects.isPendingDestroy(id);
    if (ownerChanged && !m_world.m_objects.changeOwner(
            id, *teamOwner, confirmedTick, pendingDestroyNow)) {
        return false;
    }
    if (ownerChanged && !pendingDestroy)
    {
        for (const ObjectId dependent : containmentCaptureDependents) {
            static_cast<void>(transferObjectToTeam(
                dependent, team, confirmedTick));
        }
        const PlayerState* newOwnerState = m_content.m_players.get(*teamOwner);
        const UpgradeMask completedUpgrades =
            newOwnerState ? newOwnerState->upgrades.completed
                          : UpgradeMask{};
        m_world.m_objectSimulation.onObjectOwnerChanged(
            m_world.m_registry, m_world.m_objects, id, completedUpgrades,
            confirmedTick,
            {.players = &m_content.m_players,
             .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
             .content = &m_content.m_contentSnapshot,
             .random = &m_content.m_simulationRandom,
             .effects = &m_world.m_objectSimulation});
        static_cast<void>(applyDifficultyBonusPolicy(
            id, m_presentation.m_objectsReceiveDifficultyBonuses, confirmedTick));
        recordRecoveredVehicleAcademy(
            m_content.m_players, m_world.m_registry, *entity, previousObjectOwner,
            *teamOwner, confirmedTick);
        recordCapturedObjectScore(
            m_content.m_players, m_world.m_registry, *entity, *teamOwner);
    }
    if (ownerChanged)
        refreshDerivedAggregates(confirmedTick);
    if (ownerChanged) publishLifecycleAfterOwnerChange();
    return true;
}

bool GameSessionObjectOwnershipTransactions::transferTeamOwnership(
    ObjectTeamId team, PlayerId owner, uint64_t confirmedTick) {
    if (!m_content.m_active || !team || !m_content.m_players.get(owner)) return false;
    const std::optional<PlayerId> previousOwner =
        m_world.m_objectTeams.teamOwner(team);
    if (!previousOwner || *previousOwner == owner) return false;

    const container::Span<const ObjectId> liveMembers =
        m_world.m_objectTeams.legacyMembers(team);
    container::Vector<ObjectId> members{liveMembers.begin(), liveMembers.end()};
    // Validate every entity before mutating either the Team record or one
    // OwnerComponent. A malformed registry must reject the entire transfer,
    // never leave a prefix of a team under a different player.
    for (const ObjectId member : members) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        const OwnerComponent* memberOwner = entity
            ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
            : nullptr;
        const PrimaryTeamComponent* memberTeam = entity
            ? ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!entity || !memberOwner || !memberTeam ||
            memberOwner->player != *previousOwner ||
            memberTeam->team != team) {
            return false;
        }
    }

    if (!m_world.m_objectTeams.setTeamOwner(team, owner)) return false;
    const PlayerState* newOwnerState = m_content.m_players.get(owner);
    const UpgradeMask completedUpgrades =
        newOwnerState ? newOwnerState->upgrades.completed
                      : UpgradeMask{};
    const bool idleCapturedMembers =
        m_content.m_players.relationship(
            *previousOwner, owner) != PlayerRelationship::Allies;
    for (const ObjectId member : members) {
        // ZH Team::setControllingPlayer invokes Object::onCapture for every
        // member.  Preserve its AI-idle boundary here as well as in the
        // single-object transfer path: an enemy Team reassignment must not
        // carry the previous controller's move/build/attack task into the
        // new ownership domain.
        if (idleCapturedMembers) {
            const std::optional<ecs::entity> memberEntity =
                m_world.m_objects.entityFromId(member);
            ObjectOrderQueueComponent* queue = memberEntity
                ? ecs::try_get<ObjectOrderQueueComponent>(
                      m_world.m_registry, *memberEntity)
                : nullptr;
            if (!queue && memberEntity && m_ai.m_objectAI.find(member)) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    m_world.m_registry, *memberEntity);
            }
            if (queue) {
                queue->orders.clear();
                ++queue->revision;
                ++queue->externalRevision;
                if (queue->externalRevision == 0)
                    ++queue->externalRevision;
                if (m_ai.m_objectAI.find(member)) {
                    static_cast<void>(m_ai.m_objectAI.
                        synchronizeOrderExternalRevision(
                            member, queue->externalRevision));
                    static_cast<void>(m_ai.m_objectAI.
                        clearSubjectTransients(member));
                }
            }
        }
        // All members were validated against the previous owner above, so
        // this runs before any OwnerComponent changes and cannot refund to
        // the capturing player by accident.
        static_cast<void>(
            m_world.m_objectProduction.cancelAndRefundForOwnershipTransfer(
                m_world.m_registry, m_world.m_objects, m_content.m_players,
                member));
        // The up-front validation pass cannot cover this: an earlier member's
        // onObjectOwnerChanged fan-out (or its production refund) can kill or
        // pending-destroy a later member, and changeOwner then legitimately
        // fails.  Returning false here left the team record and every
        // already-processed member under the NEW owner while telling the caller
        // the transfer failed — a team split across two players with no
        // rollback.  Skip the member that can no longer be transferred and
        // finish the transaction so team and members stay consistent.
        if (!m_world.m_objects.changeOwner(member, owner, confirmedTick)) {
            continue;
        }
        m_world.m_objectSimulation.onObjectOwnerChanged(
            m_world.m_registry, m_world.m_objects, member, completedUpgrades,
            confirmedTick,
            {.players = &m_content.m_players,
             .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
             .content = &m_content.m_contentSnapshot,
             .random = &m_content.m_simulationRandom,
             .effects = &m_world.m_objectSimulation});
        static_cast<void>(applyDifficultyBonusPolicy(
            member, m_presentation.m_objectsReceiveDifficultyBonuses,
            confirmedTick));
        const std::optional<ecs::entity> memberEntity =
            m_world.m_objects.entityFromId(member);
        if (memberEntity) {
            recordRecoveredVehicleAcademy(
                m_content.m_players, m_world.m_registry, *memberEntity,
                *previousOwner, owner, confirmedTick);
            recordCapturedObjectScore(
                m_content.m_players, m_world.m_registry, *memberEntity, owner);
        }
    }
    refreshDerivedAggregates(confirmedTick);
    publishLifecycleAfterOwnerChange();
    return true;
}

void GameSessionObjectOwnershipTransactions::projectTeamRelationshipPolicy(
    ObjectTeamId team) {
    const container::SharedPtr<const ObjectRelationshipOverridePolicy> policy =
        m_world.m_objectTeams.relationshipPolicy(team);
    for (const ObjectId member : m_world.m_objectTeams.members(team)) {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(member);
        if (!entity) continue;
        if (policy) {
            if (ObjectRelationshipOverrideComponent* current =
                    ecs::try_get<ObjectRelationshipOverrideComponent>(
                        m_world.m_registry, *entity)) {
                current->policy = policy;
            } else {
                ecs::emplace<ObjectRelationshipOverrideComponent>(
                    m_world.m_registry, *entity,
                    ObjectRelationshipOverrideComponent{.policy = policy});
            }
        } else {
            ecs::remove<ObjectRelationshipOverrideComponent>(
                m_world.m_registry, *entity);
        }
    }
}

} // namespace engine
