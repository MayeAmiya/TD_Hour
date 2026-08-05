#include "game/session/transaction/GameSessionAIResolutionTransactions.h"
#include "game/session/state/GameSessionDomainState.h"

#include "debug/debug.h"
#include "game/object/simulation/runtime/ObjectAIOpportunityTargetPolicy.h"
#include "game/object/simulation/runtime/ObjectAIInsignificantBuildingPolicy.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/combat/ObjectCombatTargetability.h"
#include "game/terrain/MapVisibilityAuthority.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {

void GameSessionAIResolutionTransactions::resolveOpportunityQueries() {
    ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
    bool allConsumed = true;
    for (const ai::AIOpportunityAttackMoveQueryCommand& command :
         transients.opportunityAttackMoveQueryCommands()) {
        if (command.kind ==
            ai::AIOpportunityAttackMoveQueryCommandKind::Cancel) {
            continue;
        }
        if (!command.correlation.orderIdentity.isValid()) {
            // Production output must have crossed ObjectAIRuntime's active
            // identity gate. An uncorrelated value can never be made safe by
            // guessing from the current queue head.
            continue;
        }

        ai::AIOpportunityAttackMoveQueryFeedback feedback{
            .correlation = command.correlation,
            .kind = ai::AIOpportunityAttackMoveQueryFeedbackKind::Unsupported,
            .confirmedTick = m_presentation.m_confirmedTick,
        };
        const std::optional<ecs::entity> subjectEntity =
            m_world.m_objects.entityFromId(command.correlation.subject);
        const TransformComponent* subjectTransform = subjectEntity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *subjectEntity)
            : nullptr;
        if (subjectEntity && subjectTransform &&
            !m_policy.rejectsOrdersWhileSleeping(
                command.correlation.subject)) {
            const LogicFixedVec3 subjectFixed =
                readAuthoritativeObjectPosition(
                    m_world.m_registry, *subjectEntity, *subjectTransform);
            const ai::AIFixedPosition subjectPosition{
                .xRaw = subjectFixed.x.raw(),
                .yRaw = subjectFixed.y.raw(),
                .zRaw = subjectFixed.z.raw(),
            };
            const OwnerComponent* subjectOwner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *subjectEntity);
            const ObjectMapStatusComponent* subjectMapStatus =
                ecs::try_get<ObjectMapStatusComponent>(
                    m_world.m_registry, *subjectEntity);
            const bool subjectOffMap =
                subjectMapStatus && subjectMapStatus->offMap;
            math::q32_32 acquisitionRange =
                effectiveObjectVisionRangeFixed(
                    m_world.m_registry, *subjectEntity);
            const PlayerState* subjectPlayer = subjectOwner
                ? m_content.m_players.get(subjectOwner->player) : nullptr;
            const ThingTemplateComponent* subjectType =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *subjectEntity);
            const game::ObjectAIBehaviorPlan* subjectBehavior =
                subjectType && subjectType->archetype
                ? subjectType->archetype->aiBehaviorPlan.get() : nullptr;
            const bool acquireBuildings =
                (subjectPlayer && subjectPlayer->controller ==
                    PlayerControllerKind::Ai) ||
                (subjectBehavior &&
                 (subjectBehavior->autoAcquireEnemiesWhenIdle &
                 game::ObjectAIAutoAcquireAttackBuildings) != 0);
            const bool humanControlled = subjectPlayer &&
                subjectPlayer->controller == PlayerControllerKind::Human;
            const container::SharedPtr<const game::terrain::MapVisibilitySnapshot>
                visibility = humanControlled
                    ? m_world.m_mapVisibility.snapshot() : nullptr;
            const ObjectAIBehaviorPolicyComponent* subjectPolicy =
                ecs::try_get<ObjectAIBehaviorPolicyComponent>(
                    m_world.m_registry, *subjectEntity);
            if (subjectPlayer &&
                subjectPlayer->controller == PlayerControllerKind::Ai &&
                subjectPolicy) {
                if (subjectPolicy->attitude == ObjectAIAttitude::Sleep)
                    acquisitionRange = {};
                else if (subjectPolicy->attitude == ObjectAIAttitude::Alert)
                    acquisitionRange *=
                        m_content.m_objectSimulationRules.ai.alertRangeModifier;
                else if (subjectPolicy->attitude ==
                         ObjectAIAttitude::Aggressive)
                    acquisitionRange *= m_content.m_objectSimulationRules.ai
                        .aggressiveRangeModifier;
            }
            ObjectAIOpportunityTargetSelection selection;
            ObjectId commonTeamTarget = INVALID_OBJECT_ID;
            if (m_presentation.m_scenarioDefinition &&
                (!subjectPolicy || subjectPolicy->attitude >=
                    ObjectAIAttitude::Normal)) {
                const std::optional<ObjectTeamId> team =
                    m_world.m_objectTeams.teamOf(
                        command.correlation.subject);
                const ObjectTeamRecord* teamRecord = team
                    ? m_world.m_objectTeams.find(*team) : nullptr;
                const scenario::ScriptTeamDefinition* teamDefinition =
                    teamRecord && teamRecord->scenarioDefinition
                    ? m_presentation.m_scenarioDefinition->findScriptTeam(
                          teamRecord->scenarioDefinition)
                    : nullptr;
                const ObjectId sharedTarget = teamDefinition &&
                        teamDefinition->plan.attackCommonTarget
                    ? teamRecord->commonTarget : INVALID_OBJECT_ID;
                const std::optional<ecs::entity> sharedEntity = sharedTarget
                    ? m_world.m_objects.entityFromId(sharedTarget)
                    : std::nullopt;
                const ObjectHealthComponent* sharedHealth = sharedEntity
                    ? ecs::try_get<ObjectHealthComponent>(
                          m_world.m_registry, *sharedEntity)
                    : nullptr;
                const ObjectLifecycleComponent* sharedLifecycle = sharedEntity
                    ? ecs::try_get<ObjectLifecycleComponent>(
                          m_world.m_registry, *sharedEntity)
                    : nullptr;
                const ObjectMapStatusComponent* sharedMapStatus = sharedEntity
                    ? ecs::try_get<ObjectMapStatusComponent>(
                          m_world.m_registry, *sharedEntity)
                    : nullptr;
                const ObjectCombatTargetability sharedTargetability =
                    sharedEntity
                    ? queryObjectCombatTargetability(
                          m_world.m_registry, m_world.m_objects,
                          m_content.m_contentSnapshot,
                          command.correlation.subject, sharedTarget,
                          m_presentation.m_confirmedTick)
                    : ObjectCombatTargetability{};
                if (sharedEntity && sharedLifecycle &&
                    sharedLifecycle->phase == ObjectLifecyclePhase::Alive &&
                    !m_world.m_objects.isPendingDestroy(sharedTarget) &&
                    ((sharedMapStatus && sharedMapStatus->offMap) ==
                     subjectOffMap) &&
                    (!sharedHealth || !sharedHealth->effectivelyDead) &&
                    sharedTargetability.canAttack &&
                    relationshipBetweenObjects(
                        m_world.m_registry, m_content.m_players,
                        *subjectEntity, *sharedEntity) ==
                        PlayerRelationship::Enemies) {
                    selection = {
                        .target = sharedTarget,
                        .distanceSquared = 0,
                        .effectiveAttackPriority =
                            std::numeric_limits<int64_t>::max(),
                        .rawAttackPriority =
                            std::numeric_limits<int32_t>::max(),
                    };
                    commonTeamTarget = sharedTarget;
                }
            }
            bool passive = false;
            const std::optional<ObjectId> retaliationTarget =
                m_policy.passiveRetaliationTarget(
                    command.correlation.subject, passive);
            for (const ObjectSpatialRecord& record : m_world.m_spatialIndex.records()) {
                if (!record.object ||
                    record.object == command.correlation.subject ||
                    (passive &&
                     (!retaliationTarget ||
                      record.object != *retaliationTarget))) {
                    continue;
                }
                const std::optional<ecs::entity> targetEntity =
                    m_world.m_objects.entityFromId(record.object);
                if (!targetEntity) continue;
                const ObjectLifecycleComponent* targetLifecycle =
                    ecs::try_get<ObjectLifecycleComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectMapStatusComponent* targetMapStatus =
                    ecs::try_get<ObjectMapStatusComponent>(
                        m_world.m_registry, *targetEntity);
                if (!targetLifecycle ||
                    targetLifecycle->phase != ObjectLifecyclePhase::Alive ||
                    m_world.m_objects.isPendingDestroy(record.object) ||
                    ((targetMapStatus && targetMapStatus->offMap) !=
                     subjectOffMap)) {
                    continue;
                }
                const TransformComponent* targetTransform =
                    ecs::try_get<TransformComponent>(
                        m_world.m_registry, *targetEntity);
                if (!targetTransform) continue;
                const LogicFixedVec3 targetPosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry,
                        *targetEntity, *targetTransform);
                const ObjectHealthComponent* targetHealth =
                    ecs::try_get<ObjectHealthComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectContainedByComponent* targetContained =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *targetEntity);
                const OwnerComponent* targetOwner =
                    ecs::try_get<OwnerComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectKindOfComponent* targetKinds =
                    ecs::try_get<ObjectKindOfComponent>(
                        m_world.m_registry, *targetEntity);
                bool sameOwner = false;
                bool allied = false;
                bool enemy = false;
                if (subjectOwner && targetOwner) {
                    sameOwner =
                        subjectOwner->player == targetOwner->player;
                    const PlayerRelationship relationship =
                        relationshipBetweenObjects(
                            m_world.m_registry, m_content.m_players, *subjectEntity,
                            *targetEntity);
                    allied = relationship == PlayerRelationship::Allies;
                    enemy = relationship == PlayerRelationship::Enemies;
                }
                const bool targetStructure = targetKinds &&
                    game::objectHasKind(targetKinds->mask,
                                        game::ObjectKindOf::Structure);
                const bool targetBaseDefense = targetKinds &&
                    game::objectHasKind(targetKinds->mask,
                                        game::ObjectKindOf::FsBaseDefense);
                const bool armedContainer =
                    ecs::try_get<ObjectContainmentRuntimeComponent>(
                        m_world.m_registry, *targetEntity) &&
                    targetKinds && game::objectHasKind(
                        targetKinds->mask, game::ObjectKindOf::CanAttack);
                const ObjectCombatTargetability targetability =
                    queryObjectCombatTargetability(
                        m_world.m_registry, m_world.m_objects,
                        m_content.m_contentSnapshot,
                        command.correlation.subject, record.object,
                        m_presentation.m_confirmedTick);
                const ObjectGeometryComponent* targetGeometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        m_world.m_registry, *targetEntity);
                const math::q32_32 targetRadius = targetGeometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          targetGeometry->boundingCircleRadiusFixed)
                    : math::q32_32{};
                const bool clearForHuman = !humanControlled || !visibility ||
                    !visibility->renderingActive ||
                    (subjectOwner && visibility->footprintHasClearCellRaw(
                        subjectOwner->player, targetPosition.x.raw(),
                        targetPosition.y.raw(), targetRadius.raw()));
                considerObjectAIOpportunityTarget(
                    command.kind, subjectPosition,
                    ObjectAIOpportunityTargetCandidate{
                        .target = record.object,
                        .position = {
                            .xRaw = targetPosition.x.raw(),
                            .yRaw = targetPosition.y.raw(),
                            .zRaw = targetPosition.z.raw(),
                        },
                        .attackable = targetHealth &&
                            targetHealth->acceptsDamage &&
                            targetability.canAttack,
                        .unattackable = targetKinds && game::objectHasKind(
                            targetKinds->mask,
                            game::ObjectKindOf::Unattackable),
                        .effectivelyDead = targetHealth &&
                            targetHealth->effectivelyDead,
                        .containedPassenger = targetContained &&
                            targetContained->enclosing,
                        .hiddenStealth =
                            objectHiddenFromObserverForAcquisition(
                                m_world.m_registry, m_world.m_objects,
                                m_content.m_players, *subjectEntity,
                                *targetEntity),
                        .sameOwner = sameOwner,
                        .allied = allied,
                        .enemy = enemy,
                        .crate = command.kind ==
                                ai::AIOpportunityAttackMoveQueryCommandKind::FindCrate &&
                            canObjectAIAutonomouslyPickUpCrate(
                            m_world.m_registry,
                            m_world.m_objects,
                            m_content.m_terrain,
                            m_content.m_players,
                            m_content.m_objectSimulationRules,
                            command.correlation.subject, record.object),
                        .rejectedByAcquirePolicy = targetStructure &&
                            !acquireBuildings && !targetBaseDefense &&
                            !armedContainer,
                        .rejectedByTargetability = humanControlled &&
                            (!targetability.withinAnyWeaponRange ||
                             !clearForHuman),
                        .ignoredInsignificantBuilding =
                            m_content.m_objectSimulationRules.ai
                                    .attackIgnoreInsignificantBuildings &&
                            isObjectAIIgnoredInsignificantBuilding(
                                m_world.m_registry, *targetEntity,
                                targetKinds),
                        .attackPriority =
                            m_policy.attackPriorityForTarget(
                                command.correlation.subject,
                                *targetEntity),
                        .attackPriorityDistanceModifierRaw =
                            m_content.m_objectSimulationRules.ai
                                .attackPriorityDistanceModifier.raw(),
                        .maximumAcquisitionDistanceRaw =
                            acquisitionRange.raw(),
                    },
                    selection);
            }
            feedback.target = selection.target;
            feedback.commonTeamTarget = selection.target &&
                selection.target == commonTeamTarget;
            if (selection.target) {
                const std::optional<ObjectTeamId> team =
                    m_world.m_objectTeams.teamOf(
                        command.correlation.subject);
                const ObjectTeamRecord* teamRecord = team
                    ? m_world.m_objectTeams.find(*team) : nullptr;
                const scenario::ScriptTeamDefinition* teamDefinition =
                    teamRecord && teamRecord->scenarioDefinition &&
                        m_presentation.m_scenarioDefinition
                    ? m_presentation.m_scenarioDefinition->findScriptTeam(
                          teamRecord->scenarioDefinition)
                    : nullptr;
                if (teamDefinition &&
                    teamDefinition->plan.attackCommonTarget) {
                    static_cast<void>(
                        m_world.m_objectTeams.setCommonTarget(
                            *team, selection.target));
                }
                const std::optional<ecs::entity> selectedEntity =
                    m_world.m_objects.entityFromId(
                        selection.target);
                const TransformComponent* selectedTransform = selectedEntity
                    ? ecs::try_get<TransformComponent>(
                          m_world.m_registry,
                          *selectedEntity)
                    : nullptr;
                if (selectedEntity && selectedTransform) {
                    const LogicFixedVec3 selected =
                        readAuthoritativeObjectPosition(
                            m_world.m_registry,
                            *selectedEntity, *selectedTransform);
                    feedback.targetPosition = {
                        .xRaw = selected.x.raw(),
                        .yRaw = selected.y.raw(),
                        .zRaw = selected.z.raw(),
                    };
                    feedback.targetPositionValid = true;
                }
            }
            feedback.kind = selection.target
                ? ai::AIOpportunityAttackMoveQueryFeedbackKind::Target
                : ai::AIOpportunityAttackMoveQueryFeedbackKind::NoTarget;
        }
        const ai::ObjectAITransientStatus staged = transients.stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            allConsumed = false;
            TD_LOG_ERROR(
                "[GameSession] Object AI AttackMove query feedback "
                "rejected: subject={} tick={} status={}",
                command.correlation.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
        }
    }
    // Successful feedback values remain invisible until the next AI phase.
    // If capacity rejected any value, retain all commands; already-staged
    // feedback is replaced by subject on retry and cannot duplicate effects.
    if (allConsumed)
        transients.discardOpportunityAttackMoveQueryCommands();
}

void GameSessionAIResolutionTransactions::resolveTacticalAttackQueries() {
    ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
    // AIHuntState::onExit releases only a temporary lock. StartOrReplace
    // child commands stay inside the runtime; EndWrapper is the narrow
    // mutation request that crosses into the ECS-owning simulation boundary.
    for (const ai::AITacticalAttackChildCommand& command :
         transients.tacticalAttackChildCommands()) {
        if (command.kind !=
                ai::AITacticalAttackChildCommandKind::EndWrapper ||
            !command.releaseTemporaryWeaponLock) {
            continue;
        }
        const std::optional<ecs::entity> subject =
            m_world.m_objects.entityFromId(command.correlation.subject);
        if (subject) {
            static_cast<void>(releaseObjectWeaponLock(
                m_world.m_registry, *subject,
                ObjectWeaponLockType::Temporary));
        }
    }
    transients.discardTacticalAttackChildCommands();

    bool allConsumed = true;
    for (const ai::AITacticalAttackQueryCommand& command :
         transients.tacticalAttackQueryCommands()) {
        bool randomConsumed = false;
        if (command.kind ==
            ai::AITacticalAttackQueryCommandKind::Cancel) {
            continue;
        }
        ai::AITacticalAttackQueryFeedback feedback{
            .correlation = command.correlation,
            .status = ai::AITacticalAttackQueryStatus::Unsupported,
            .confirmedTick = m_presentation.m_confirmedTick,
        };
        const bool crateQuery = command.correlation.query ==
            ai::AITacticalAttackQueryKind::Crate;
        const bool huntQuery = command.correlation.query ==
            ai::AITacticalAttackQueryKind::HuntTarget;
        const bool squadQuery = command.correlation.query ==
            ai::AITacticalAttackQueryKind::SquadTarget;
        const bool areaQuery = command.correlation.query ==
            ai::AITacticalAttackQueryKind::AreaTarget;
        ObjectTeamId targetTeam = INVALID_OBJECT_TEAM_ID;
        container::Span<const ObjectId> targetTeamMembers;
        container::Span<const ObjectId> targetTeamLegacyMembers;
        const game::terrain::PolygonTriggerRecord* targetArea = nullptr;
        bool domainValid = crateQuery || huntQuery;
        if (squadQuery && command.correlation.collection.value <=
                std::numeric_limits<uint32_t>::max()) {
            targetTeam = ObjectTeamId{
                static_cast<uint32_t>(
                    command.correlation.collection.value)};
            targetTeamMembers = m_world.m_objectTeams.members(targetTeam);
            targetTeamLegacyMembers = m_world.m_objectTeams.legacyMembers(targetTeam);
            domainValid = targetTeam &&
                m_world.m_objectTeams.find(targetTeam) &&
                m_world.m_objectTeams.membershipRevision(targetTeam) ==
                    command.correlation.collectionRevision;
        } else if (areaQuery && command.correlation.area.value != 0 &&
                   command.correlation.area.value - 1 <=
                       std::numeric_limits<uint32_t>::max()) {
            targetArea = m_content.m_terrain.triggerById(static_cast<uint32_t>(
                command.correlation.area.value - 1));
            domainValid = targetArea &&
                game::terrain::TerrainLogic::triggerRevision(*targetArea) ==
                    command.correlation.areaRevision;
        }
        if (!command.correlation.orderIdentity.isValid() ||
            !domainValid) {
            const ai::ObjectAITransientStatus staged =
                transients.stage(feedback);
            allConsumed = allConsumed &&
                staged == ai::ObjectAITransientStatus::Success;
            continue;
        }

        const std::optional<ecs::entity> subjectEntity =
            m_world.m_objects.entityFromId(command.correlation.subject);
        const TransformComponent* subjectTransform = subjectEntity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *subjectEntity)
            : nullptr;
        if (subjectEntity && subjectTransform) {
            // RefCode reads ScriptEngine::getChooseVictimAlwaysUsesNormal()
            // each time AttackSquad chooses another victim, not only when
            // the order begins. Re-resolve here so a mid-order script toggle
            // affects the very next SquadTarget query.
            const ai::AISquadTargetSelection squadSelection = squadQuery
                ? m_policy.squadTargetSelection(
                      command.correlation.subject,
                      command.correlation.orderIdentity.source ==
                          static_cast<uint8_t>(
                              ai::ObjectAIOrderSource::Player))
                : command.squadSelection;
            const LogicFixedVec3 subjectFixed =
                readAuthoritativeObjectPosition(
                    m_world.m_registry, *subjectEntity, *subjectTransform);
            const ai::AIFixedPosition subjectPosition{
                .xRaw = subjectFixed.x.raw(),
                .yRaw = subjectFixed.y.raw(),
                .zRaw = subjectFixed.z.raw(),
            };
            const OwnerComponent* subjectOwner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *subjectEntity);
            const ObjectMapStatusComponent* subjectMapStatus =
                ecs::try_get<ObjectMapStatusComponent>(
                    m_world.m_registry, *subjectEntity);
            const bool subjectOffMap =
                subjectMapStatus && subjectMapStatus->offMap;
            ObjectAIOpportunityTargetSelection selection;
            ObjectAIOpportunityTargetSelection fallbackSelection;
            ObjectAIOpportunityTargetSelection commonTargetSelection;
            container::Vector<ObjectAIOpportunityTargetCandidate>
                squadCandidates;
            std::optional<ObjectTeamId> commonTargetTeam;
            ObjectId commonTarget = INVALID_OBJECT_ID;
            const bool useHuntCommonTarget =
                huntQuery && command.useTeamCommonTarget;
            if (useHuntCommonTarget) {
                commonTargetTeam = m_world.m_objectTeams.teamOf(
                    command.correlation.subject);
                const ObjectTeamRecord* teamRecord = commonTargetTeam
                    ? m_world.m_objectTeams.find(*commonTargetTeam)
                    : nullptr;
                commonTarget = teamRecord
                    ? teamRecord->commonTarget : INVALID_OBJECT_ID;
            }
            const ai::AIOpportunityAttackMoveQueryCommandKind queryKind =
                crateQuery
                    ? ai::AIOpportunityAttackMoveQueryCommandKind::FindCrate
                    : ai::AIOpportunityAttackMoveQueryCommandKind::
                          FindMoodTarget;
            for (const ObjectSpatialRecord& record :
                 m_world.m_spatialIndex.records()) {
                if (!record.object ||
                    record.object == command.correlation.subject) {
                    continue;
                }
                const std::optional<ecs::entity> targetEntity =
                    m_world.m_objects.entityFromId(record.object);
                if (!targetEntity) continue;
                const ObjectLifecycleComponent* targetLifecycle =
                    ecs::try_get<ObjectLifecycleComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectMapStatusComponent* targetMapStatus =
                    ecs::try_get<ObjectMapStatusComponent>(
                        m_world.m_registry, *targetEntity);
                if (!targetLifecycle ||
                    targetLifecycle->phase != ObjectLifecyclePhase::Alive ||
                    m_world.m_objects.isPendingDestroy(record.object) ||
                    ((huntQuery || areaQuery) &&
                     ((targetMapStatus && targetMapStatus->offMap) !=
                      subjectOffMap))) {
                    continue;
                }
                const TransformComponent* targetTransform =
                    ecs::try_get<TransformComponent>(
                        m_world.m_registry, *targetEntity);
                if (!targetTransform) continue;
                if (squadQuery &&
                    !std::binary_search(
                        targetTeamMembers.begin(), targetTeamMembers.end(),
                        record.object)) {
                    continue;
                }
                const LogicFixedVec3 targetPosition =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry,
                        *targetEntity, *targetTransform);
                if (areaQuery &&
                    !m_content.m_terrain.isInsideTriggerRaw(
                        *targetArea, targetPosition.x.raw(),
                        targetPosition.y.raw())) {
                    continue;
                }
                const ObjectHealthComponent* targetHealth =
                    ecs::try_get<ObjectHealthComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectContainedByComponent* targetContained =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *targetEntity);
                const OwnerComponent* targetOwner =
                    ecs::try_get<OwnerComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectKindOfComponent* targetKinds =
                    ecs::try_get<ObjectKindOfComponent>(
                        m_world.m_registry, *targetEntity);
                bool sameOwner = false;
                bool allied = false;
                bool enemy = false;
                if (subjectOwner && targetOwner) {
                    sameOwner =
                        subjectOwner->player == targetOwner->player;
                    const PlayerRelationship relationship =
                        relationshipBetweenObjects(
                            m_world.m_registry, m_content.m_players, *subjectEntity,
                            *targetEntity);
                    allied = relationship == PlayerRelationship::Allies;
                    enemy = relationship == PlayerRelationship::Enemies;
                }
                ObjectAIOpportunityTargetCandidate candidate{
                    .target = record.object,
                    .position = {
                        .xRaw = targetPosition.x.raw(),
                        .yRaw = targetPosition.y.raw(),
                        .zRaw = targetPosition.z.raw(),
                    },
                    .attackable = !command.canAttackOnly ||
                        (targetHealth && targetHealth->acceptsDamage),
                    .unattackable = targetKinds && game::objectHasKind(
                        targetKinds->mask,
                        game::ObjectKindOf::Unattackable),
                    .effectivelyDead = targetHealth &&
                        targetHealth->effectivelyDead,
                    .containedPassenger = targetContained &&
                        targetContained->enclosing,
                    .hiddenStealth =
                        objectHiddenFromObserverForAcquisition(
                            m_world.m_registry, m_world.m_objects,
                            m_content.m_players, *subjectEntity,
                            *targetEntity),
                    .sameOwner = sameOwner,
                    .allied = allied,
                    .enemy = enemy,
                    .crate = crateQuery &&
                        canObjectAIAutonomouslyPickUpCrate(
                            m_world.m_registry, m_world.m_objects,
                            m_content.m_terrain, m_content.m_players,
                            m_content.m_objectSimulationRules,
                            command.correlation.subject, record.object),
                    .attackPriority = command.useAttackPriority
                        ? m_policy.attackPriorityForTarget(
                              command.correlation.subject, *targetEntity)
                        : 1,
                    .attackPriorityDistanceModifierRaw =
                        m_content.m_objectSimulationRules.ai
                            .attackPriorityDistanceModifier.raw(),
                };
                if (squadQuery) {
                    squadCandidates.push_back(candidate);
                } else {
                    considerObjectAIOpportunityTarget(
                        queryKind, subjectPosition, candidate, selection);
                    if (useHuntCommonTarget &&
                        record.object == commonTarget) {
                        considerObjectAIOpportunityTarget(
                            queryKind, subjectPosition, candidate,
                            commonTargetSelection);
                    }
                    if (command.fallbackWithoutAttackPriority) {
                        candidate.attackPriority = 1;
                        considerObjectAIOpportunityTarget(
                            queryKind, subjectPosition, candidate,
                            fallbackSelection);
                    }
                }
            }

            ObjectId selectedTarget = selection.target;
            if (squadQuery) {
                const auto liveLegacyMembers = [&]() {
                    container::Vector<ObjectId> live;
                    live.reserve(targetTeamLegacyMembers.size());
                    for (const ObjectId member : targetTeamLegacyMembers) {
                        const std::optional<ecs::entity> memberEntity =
                            m_world.m_objects.entityFromId(member);
                        const ObjectHealthComponent* health = memberEntity
                            ? ecs::try_get<ObjectHealthComponent>(
                                  m_world.m_registry, *memberEntity)
                            : nullptr;
                        const ObjectKindOfComponent* kinds = memberEntity
                            ? ecs::try_get<ObjectKindOfComponent>(
                                  m_world.m_registry, *memberEntity)
                            : nullptr;
                        const ObjectStatusComponent* status = memberEntity
                            ? ecs::try_get<ObjectStatusComponent>(
                                  m_world.m_registry, *memberEntity)
                            : nullptr;
                        const bool alwaysSelectable = kinds &&
                            game::objectHasKind(
                                kinds->mask,
                                game::ObjectKindOf::AlwaysSelectable);
                        const bool authoredSelectable = kinds &&
                            game::objectHasKind(
                                kinds->mask,
                                game::ObjectKindOf::Selectable);
                        const bool unselectable = status && status->hasAny(
                            game::objectStatusBit(
                                game::ObjectStatusFlag::Unselectable));
                        const bool effectivelyDead =
                            health && health->effectivelyDead;
                        if (memberEntity &&
                            (alwaysSelectable ||
                             (authoredSelectable && !unselectable &&
                              !effectivelyDead))) {
                            live.push_back(member);
                        }
                    }
                    return live;
                };
                switch (squadSelection) {
                case ai::AISquadTargetSelection::NoTarget:
                    selectedTarget = INVALID_OBJECT_ID;
                    break;
                case ai::AISquadTargetSelection::FirstLiveMember: {
                    const container::Vector<ObjectId> live =
                        liveLegacyMembers();
                    selectedTarget = live.empty()
                        ? INVALID_OBJECT_ID : live.front();
                    break;
                }
                case ai::AISquadTargetSelection::RandomLiveMember: {
                    const container::Vector<ObjectId> live =
                        liveLegacyMembers();
                    if (live.empty()) {
                        selectedTarget = INVALID_OBJECT_ID;
                    } else {
                        const int32_t index =
                            m_content.m_simulationRandom.integerInclusive(
                                0, static_cast<int32_t>(live.size() - 1));
                        randomConsumed = true;
                        selectedTarget = live[static_cast<size_t>(index)];
                    }
                    break;
                }
                case ai::AISquadTargetSelection::ClosestLiveMember:
                    selectedTarget = INVALID_OBJECT_ID;
                    {
                        const ObjectMapStatusComponent* subjectMapStatus =
                            ecs::try_get<ObjectMapStatusComponent>(
                                m_world.m_registry, *subjectEntity);
                        const bool subjectOffMap =
                            subjectMapStatus && subjectMapStatus->offMap;
                        uint64_t bestDistance =
                            std::numeric_limits<uint64_t>::max();
                        for (const ObjectAIOpportunityTargetCandidate& candidate :
                             squadCandidates) {
                            if (!candidate.target ||
                                candidate.effectivelyDead) {
                                continue;
                            }
                            const std::optional<ecs::entity> candidateEntity =
                                m_world.m_objects.entityFromId(
                                    candidate.target);
                            const ObjectMapStatusComponent* candidateMapStatus =
                                candidateEntity
                                ? ecs::try_get<ObjectMapStatusComponent>(
                                      m_world.m_registry, *candidateEntity)
                                : nullptr;
                            const bool candidateOffMap = candidateMapStatus &&
                                candidateMapStatus->offMap;
                            if (!candidateEntity ||
                                candidateOffMap != subjectOffMap) {
                                continue;
                            }
                            const uint64_t distance =
                                object_ai_opportunity_detail::distanceSquared(
                                    subjectPosition, candidate.position);
                            if (!selectedTarget || distance < bestDistance ||
                                (distance == bestDistance &&
                                 candidate.target < selectedTarget)) {
                                selectedTarget = candidate.target;
                                bestDistance = distance;
                            }
                        }
                    }
                    break;
                case ai::AISquadTargetSelection::LastDamageSource: {
                    const std::optional<ObjectId> retaliation =
                        m_policy.attackSquadPassiveTarget(
                            command.correlation.subject);
                    selectedTarget = retaliation.value_or(
                        INVALID_OBJECT_ID);
                    break;
                }
                }
            } else if (!selectedTarget &&
                       command.fallbackWithoutAttackPriority) {
                selectedTarget = fallbackSelection.target;
            }
            if (useHuntCommonTarget && commonTargetSelection.target) {
                const bool hasAttackPrioritySet =
                    m_policy.hasExplicitAttackPrioritySet(
                        command.correlation.subject);
                int32_t selectedPriority = 0;
                if (selection.target == selectedTarget) {
                    selectedPriority = selection.rawAttackPriority;
                } else if (selectedTarget) {
                    const std::optional<ecs::entity> selectedEntity =
                        m_world.m_objects.entityFromId(selectedTarget);
                    if (selectedEntity) {
                        selectedPriority = m_policy.attackPriorityForTarget(
                            command.correlation.subject, *selectedEntity);
                    }
                }
                // AIHuntState uses the Team victim immediately when no
                // AttackPriorityInfo is installed. With an explicit table it
                // compares raw authored priorities after the ordinary scan;
                // equality deliberately favors the already-shared victim.
                if (!hasAttackPrioritySet || !selectedTarget ||
                    commonTargetSelection.rawAttackPriority >=
                        selectedPriority) {
                    selectedTarget = commonTargetSelection.target;
                }
            }
            if (useHuntCommonTarget && commonTargetTeam) {
                // RefCode writes the final Hunt result back even when it is
                // null, clearing a dead, hidden, or otherwise illegal shared
                // victim instead of leaving the Team pinned to stale state.
                static_cast<void>(m_world.m_objectTeams.setCommonTarget(
                    *commonTargetTeam, selectedTarget));
            }
            feedback.status = ai::AITacticalAttackQueryStatus::Completed;
            feedback.target = selectedTarget;
            if (selectedTarget) {
                const std::optional<ecs::entity> selectedEntity =
                    m_world.m_objects.entityFromId(selectedTarget);
                const ObjectLifecycleComponent* selectedLifecycle =
                    selectedEntity
                    ? ecs::try_get<ObjectLifecycleComponent>(
                          m_world.m_registry, *selectedEntity)
                    : nullptr;
                const TransformComponent* selectedTransform = selectedEntity
                    ? ecs::try_get<TransformComponent>(
                          m_world.m_registry, *selectedEntity)
                    : nullptr;
                if (selectedLifecycle) {
                    // ObjectId is never reused, while createdAtTick supplies a
                    // stable non-zero lifetime token. Periodic scans selecting
                    // the same live target therefore preserve the existing
                    // AttackObject child instead of restarting it every scan.
                    feedback.targetRevision =
                        selectedLifecycle->createdAtTick ==
                                std::numeric_limits<uint64_t>::max()
                            ? std::numeric_limits<uint64_t>::max()
                            : selectedLifecycle->createdAtTick + 1;
                }
                if (selectedEntity && selectedTransform) {
                    const LogicFixedVec3 selected =
                        readAuthoritativeObjectPosition(
                            m_world.m_registry, *selectedEntity,
                            *selectedTransform);
                    feedback.targetPosition = {
                        .xRaw = selected.x.raw(),
                        .yRaw = selected.y.raw(),
                        .zRaw = selected.z.raw(),
                    };
                    feedback.targetPositionValid = true;
                }
            }
        }
        const ai::ObjectAITransientStatus staged =
            transients.stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            allConsumed = false;
            TD_LOG_ERROR(
                "[GameSession] Object AI tactical query feedback rejected: "
                "subject={} tick={} status={}",
                command.correlation.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
            if (randomConsumed) {
                static_cast<void>(m_publication.raiseSimulationFault({
                    .domain = SimulationFaultDomain::Feedback,
                    .code = staged == ai::ObjectAITransientStatus::CapacityExceeded
                        ? SimulationFaultCode::CapacityExceeded
                        : SimulationFaultCode::InvalidEvent,
                    .confirmedTick = m_presentation.m_confirmedTick,
                    .subject = command.correlation.subject.value,
                }));
                return;
            }
        }
    }
    if (allConsumed)
        transients.discardTacticalAttackQueryCommands();
}

void GameSessionAIResolutionTransactions::resolveGuardCommands() {
    ai::ObjectAITransientStore& transients = m_ai.m_objectAI.transients();
    container::Vector<ai::AIGuardTacticalCommand> consumedTactical;
    for (const ai::AIGuardTacticalCommand& command :
        transients.guardTacticalCommands()) {
        if (command.kind == ai::AIGuardTacticalCommandKind::Cancel) {
            if (command.clearTeamTarget) {
                const std::optional<ObjectTeamId> team =
                    m_world.m_objectTeams.teamOf(
                        command.correlation.subject);
                if (team) {
                    static_cast<void>(m_world.m_objectTeams.setCommonTarget(
                        *team, INVALID_OBJECT_ID));
                }
            }
            consumedTactical.push_back(command);
            continue;
        }
        if (command.kind ==
            ai::AIGuardTacticalCommandKind::BeginAttack) {
            if (command.publishTunnelNemesis && command.target &&
                command.correlation.isValid() &&
                command.correlation.orderIdentity.isValid()) {
                static_cast<void>(m_world.m_objectSimulation
                    .publishTunnelNetworkNemesis(
                        m_world.m_registry, m_world.m_objects,
                        command.correlation.subject, command.target,
                        m_presentation.m_confirmedTick));
            }
            // The Guard SoA runner has already accepted this command into the
            // shared AttackObject child family. Keep the Guard operation in
            // the kernel correlation; the high-level command itself is now
            // consumed and the child terminal feedback will return through
            // the Guard inbox.
            consumedTactical.push_back(command);
            continue;
        }
        if (command.kind == ai::AIGuardTacticalCommandKind::BeginMove) {
            // The Guard Move child has accepted this operation into the
            // shared MoveTo path/movement body. Completion returns as Guard
            // feedback; the high-level command is one-shot.
            consumedTactical.push_back(command);
            continue;
        }

        ai::AIGuardFeedback feedback{
            .correlation = command.correlation,
            .kind = ai::AIGuardFeedbackKind::Unsupported,
            .target = command.target,
            .confirmedTick = m_presentation.m_confirmedTick,
        };
        const std::optional<ecs::entity> subjectEntity =
            m_world.m_objects.entityFromId(command.correlation.subject);
        const TransformComponent* subjectTransform = subjectEntity
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *subjectEntity)
            : nullptr;
        const game::terrain::PolygonTriggerRecord* guardArea = nullptr;
        bool guardAreaValid = !command.area && command.areaRevision == 0;
        if (command.area && command.area.value - 1 <=
                std::numeric_limits<uint32_t>::max()) {
            guardArea = m_content.m_terrain.triggerById(
                static_cast<uint32_t>(command.area.value - 1));
            guardAreaValid = guardArea &&
                game::terrain::TerrainLogic::triggerRevision(*guardArea) ==
                    command.areaRevision;
        }
        if (command.kind ==
                       ai::AIGuardTacticalCommandKind::ScanForTarget &&
                   subjectEntity && subjectTransform &&
                   command.correlation.orderIdentity.isValid() &&
                   guardAreaValid &&
                   !m_policy.rejectsOrdersWhileSleeping(
                       command.correlation.subject)) {
            const OwnerComponent* subjectOwner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *subjectEntity);
            const ObjectMapStatusComponent* subjectMapStatus =
                ecs::try_get<ObjectMapStatusComponent>(
                    m_world.m_registry, *subjectEntity);
            const bool subjectOffMap =
                subjectMapStatus && subjectMapStatus->offMap;
            const ThingTemplateComponent* subjectThing =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *subjectEntity);
            const bool hijackGuard = command.enterGuardTargets &&
                subjectThing && subjectThing->archetype &&
                subjectThing->archetype->templateData.hijackGuard;
            ObjectAIOpportunityTargetSelection guardSelection;
            ai::AIFixedPosition selectedPosition;
            if (!command.enterGuardTargets &&
                m_presentation.m_scenarioDefinition) {
                const std::optional<ObjectTeamId> team =
                    m_world.m_objectTeams.teamOf(
                        command.correlation.subject);
                const ObjectTeamRecord* teamRecord = team
                    ? m_world.m_objectTeams.find(*team) : nullptr;
                const scenario::ScriptTeamDefinition* teamDefinition =
                    teamRecord && teamRecord->scenarioDefinition
                    ? m_presentation.m_scenarioDefinition->findScriptTeam(
                          teamRecord->scenarioDefinition)
                    : nullptr;
                const ObjectId sharedTarget = teamDefinition &&
                        teamDefinition->plan.attackCommonTarget
                    ? teamRecord->commonTarget : INVALID_OBJECT_ID;
                const std::optional<ecs::entity> sharedEntity = sharedTarget
                    ? m_world.m_objects.entityFromId(sharedTarget)
                    : std::nullopt;
                const TransformComponent* sharedTransform = sharedEntity
                    ? ecs::try_get<TransformComponent>(
                          m_world.m_registry, *sharedEntity)
                    : nullptr;
                const ObjectHealthComponent* sharedHealth = sharedEntity
                    ? ecs::try_get<ObjectHealthComponent>(
                          m_world.m_registry, *sharedEntity)
                    : nullptr;
                const ObjectLifecycleComponent* sharedLifecycle = sharedEntity
                    ? ecs::try_get<ObjectLifecycleComponent>(
                          m_world.m_registry, *sharedEntity)
                    : nullptr;
                const ObjectMapStatusComponent* sharedMapStatus = sharedEntity
                    ? ecs::try_get<ObjectMapStatusComponent>(
                          m_world.m_registry, *sharedEntity)
                    : nullptr;
                const ObjectCombatTargetability sharedTargetability =
                    sharedEntity
                    ? queryObjectCombatTargetability(
                          m_world.m_registry, m_world.m_objects,
                          m_content.m_contentSnapshot,
                          command.correlation.subject, sharedTarget,
                          m_presentation.m_confirmedTick)
                    : ObjectCombatTargetability{};
                if (sharedEntity && sharedTransform && sharedLifecycle &&
                    sharedLifecycle->phase == ObjectLifecyclePhase::Alive &&
                    !m_world.m_objects.isPendingDestroy(sharedTarget) &&
                    ((sharedMapStatus && sharedMapStatus->offMap) ==
                     subjectOffMap) &&
                    (!sharedHealth || !sharedHealth->effectivelyDead) &&
                    sharedTargetability.canAttack &&
                    relationshipBetweenObjects(
                        m_world.m_registry, m_content.m_players,
                        *subjectEntity, *sharedEntity) ==
                        PlayerRelationship::Enemies) {
                    const LogicFixedVec3 target =
                        readAuthoritativeObjectPosition(
                            m_world.m_registry, *sharedEntity,
                            *sharedTransform);
                    guardSelection = {
                        .target = sharedTarget,
                        .distanceSquared = 0,
                        .effectiveAttackPriority =
                            std::numeric_limits<int64_t>::max(),
                        .rawAttackPriority =
                            std::numeric_limits<int32_t>::max(),
                    };
                    selectedPosition = {
                        .xRaw = target.x.raw(),
                        .yRaw = target.y.raw(),
                        .zRaw = target.z.raw(),
                    };
                }
            }
            bool passive = false;
            const std::optional<ObjectId> retaliationTarget =
                m_policy.passiveRetaliationTarget(
                    command.correlation.subject, passive);
            const math::q32_32 anchorX =
                math::q32_32::from_raw(command.anchor.xRaw);
            const math::q32_32 anchorY =
                math::q32_32::from_raw(command.anchor.yRaw);
            const math::q32_32 radius = math::q32_32::max(
                math::q32_32{},
                math::q32_32::from_raw(command.radiusRaw));
            const math::q32_32 radiusSquared = radius * radius;
            for (const ObjectSpatialRecord& record :
                 m_world.m_spatialIndex.records()) {
                if (!record.object ||
                    record.object == command.correlation.subject ||
                    (passive &&
                     (!retaliationTarget ||
                      record.object != *retaliationTarget)))
                    continue;
                const std::optional<ecs::entity> targetEntity =
                    m_world.m_objects.entityFromId(record.object);
                if (!targetEntity) continue;
                const ObjectLifecycleComponent* targetLifecycle =
                    ecs::try_get<ObjectLifecycleComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectMapStatusComponent* targetMapStatus =
                    ecs::try_get<ObjectMapStatusComponent>(
                        m_world.m_registry, *targetEntity);
                if (!targetLifecycle ||
                    targetLifecycle->phase != ObjectLifecyclePhase::Alive ||
                    m_world.m_objects.isPendingDestroy(record.object) ||
                    ((targetMapStatus && targetMapStatus->offMap) !=
                     subjectOffMap)) {
                    continue;
                }
                const TransformComponent* targetTransform =
                    ecs::try_get<TransformComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectHealthComponent* targetHealth =
                    ecs::try_get<ObjectHealthComponent>(
                        m_world.m_registry, *targetEntity);
                const OwnerComponent* targetOwner =
                    ecs::try_get<OwnerComponent>(m_world.m_registry, *targetEntity);
                const ObjectContainedByComponent* targetContained =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectKindOfComponent* targetKinds =
                    ecs::try_get<ObjectKindOfComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectAirborneComponent* targetAirborne =
                    ecs::try_get<ObjectAirborneComponent>(
                        m_world.m_registry, *targetEntity);
                const ObjectContainmentRuntimeComponent* targetContainment =
                    ecs::try_get<ObjectContainmentRuntimeComponent>(
                        m_world.m_registry, *targetEntity);
                const bool targetStructure = targetKinds &&
                    game::objectHasKind(
                        targetKinds->mask, game::ObjectKindOf::Structure);
                const bool targetBaseDefense = targetKinds &&
                    game::objectHasKind(
                        targetKinds->mask, game::ObjectKindOf::FsBaseDefense);
                const bool targetFlying = targetAirborne &&
                    targetAirborne->isAirborne;
                const PlayerRelationship relationship =
                    subjectOwner && targetOwner
                    ? relationshipBetweenObjects(
                          m_world.m_registry, m_content.m_players,
                          *subjectEntity, *targetEntity)
                    : PlayerRelationship::Neutral;
                const bool relationAllowed = command.enterGuardTargets
                    ? hijackGuard
                        ? relationship == PlayerRelationship::Enemies
                        : relationship == PlayerRelationship::Neutral
                    : relationship == PlayerRelationship::Enemies;
                const ObjectCombatTargetability targetability =
                    !command.enterGuardTargets
                    ? queryObjectCombatTargetability(
                          m_world.m_registry, m_world.m_objects,
                          m_content.m_contentSnapshot,
                          command.correlation.subject, record.object,
                          m_presentation.m_confirmedTick)
                    : ObjectCombatTargetability{};
                const bool candidateAllowed = command.enterGuardTargets
                    ? targetContainment && targetContainment->plan &&
                        (!hijackGuard || (targetKinds &&
                            game::objectHasKind(
                                targetKinds->mask,
                                game::ObjectKindOf::Vehicle)))
                    : targetHealth && targetHealth->acceptsDamage &&
                        targetability.canAttack &&
                        !(targetKinds && game::objectHasKind(
                            targetKinds->mask,
                            game::ObjectKindOf::Unattackable)) &&
                        (!command.rejectOrdinaryBuildings ||
                         !targetStructure || targetBaseDefense);
                if (!targetTransform || !targetHealth ||
                    targetHealth->effectivelyDead || !candidateAllowed ||
                    (command.flyingOnly && !targetFlying) ||
                    (targetContained &&
                     targetContained->enclosing) ||
                    !subjectOwner || !targetOwner ||
                    !relationAllowed)
                    continue;
                if (guardArea &&
                    [&]() {
                        const LogicFixedVec3 position =
                            readAuthoritativeObjectPosition(
                                m_world.m_registry,
                                *targetEntity, *targetTransform);
                        return !m_content.m_terrain
                            .isInsideTriggerLegacyRaw(
                                *guardArea, position.x.raw(),
                                position.y.raw());
                    }()) {
                    continue;
                }
                if (objectHiddenFromObserverForAcquisition(
                        m_world.m_registry, m_world.m_objects,
                        m_content.m_players, *subjectEntity,
                        *targetEntity))
                    continue;
                const LogicFixedVec3 target =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *targetEntity, *targetTransform);
                const math::q32_32 dx = target.x - anchorX;
                const math::q32_32 dy = target.y - anchorY;
                const math::q32_32 distanceSquared = dx * dx + dy * dy;
                if (distanceSquared > radiusSquared) continue;
                const ai::AIFixedPosition targetPosition{
                    .xRaw = target.x.raw(),
                    .yRaw = target.y.raw(),
                    .zRaw = target.z.raw(),
                };
                const uint64_t candidateDistance =
                    object_ai_opportunity_detail::distanceSquared(
                        command.anchor, targetPosition);
                // AIGuardMachine::lookForInnerTarget asks PartitionManager for
                // the closest object after relationship/attackability filters.
                // AttackPriority belongs to ordinary mood acquisition and must
                // not make Guard chase a farther high-priority target. Stable
                // ObjectId is the deterministic tie-break for equal distance.
                if (!guardSelection.target ||
                    candidateDistance < guardSelection.distanceSquared ||
                    (candidateDistance == guardSelection.distanceSquared &&
                     record.object < guardSelection.target)) {
                    guardSelection.target = record.object;
                    guardSelection.distanceSquared = candidateDistance;
                    selectedPosition = {
                        .xRaw = targetPosition.xRaw,
                        .yRaw = targetPosition.yRaw,
                        .zRaw = targetPosition.zRaw,
                    };
                }
            }
            const ObjectId selected = guardSelection.target;
            if (selected && m_presentation.m_scenarioDefinition) {
                const ObjectId subject = command.correlation.subject;
                const std::optional<ObjectTeamId> team =
                    m_world.m_objectTeams.teamOf(subject);
                const ObjectTeamRecord* record = team
                    ? m_world.m_objectTeams.find(*team) : nullptr;
                const scenario::ScriptTeamDefinition* definition = record &&
                        record->scenarioDefinition
                    ? m_presentation.m_scenarioDefinition->findScriptTeam(
                          record->scenarioDefinition)
                    : nullptr;
                if (definition &&
                    definition->plan.attackCommonTarget) {
                    static_cast<void>(
                        m_world.m_objectTeams.setCommonTarget(
                            *team, selected));
                }
            }
            feedback.kind = selected
                ? ai::AIGuardFeedbackKind::Succeeded
                : ai::AIGuardFeedbackKind::Failed;
            feedback.target = selected;
            feedback.targetPosition = selectedPosition;
        }
        if (transients.stage(feedback) ==
            ai::ObjectAITransientStatus::Success) {
            consumedTactical.push_back(command);
        }
    }
    for (const ai::AIGuardTacticalCommand& command : consumedTactical) {
        static_cast<void>(transients.removeGuardTacticalCommand(
            command.correlation, command.kind));
    }

    container::Vector<ai::AIGuardInteractionCommand> consumedInteraction;
    for (const ai::AIGuardInteractionCommand& command :
        transients.guardInteractionCommands()) {
        if (command.kind == ai::AIGuardInteractionCommandKind::Cancel ||
            command.kind == ai::AIGuardInteractionCommandKind::EndGuard) {
            if (command.clearTeamTarget) {
                const std::optional<ObjectTeamId> team =
                    m_world.m_objectTeams.teamOf(
                        command.correlation.subject);
                if (team) {
                    static_cast<void>(m_world.m_objectTeams.setCommonTarget(
                        *team, INVALID_OBJECT_ID));
                }
            }
            // Both commands close kernel-owned child intent. The optional
            // Team-target clear mirrors the exiting legacy Guard substate and
            // is idempotent; neither command produces terminal feedback.
            consumedInteraction.push_back(command);
            continue;
        }

        ai::AIGuardFeedback feedback{
            .correlation = command.correlation,
            .kind = ai::AIGuardFeedbackKind::Unsupported,
            .confirmedTick = m_presentation.m_confirmedTick,
        };
        bool terminal = true;
        bool sideEffectCommitted = false;
        if (command.correlation.isValid() &&
            command.correlation.orderIdentity.isValid()) {
            const std::optional<ecs::entity> subjectEntity =
                m_world.m_objects.entityFromId(command.correlation.subject);
            switch (command.kind) {
            case ai::AIGuardInteractionCommandKind::BeginEnter: {
                const std::optional<ecs::entity> targetEntity =
                    m_world.m_objects.entityFromId(command.target);
                if (!subjectEntity || !targetEntity) {
                    feedback.kind = ai::AIGuardFeedbackKind::Failed;
                    break;
                }
                const ObjectContainedByComponent* existing =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *subjectEntity);
                if (existing) {
                    feedback.kind = existing->container == command.target
                        ? ai::AIGuardFeedbackKind::Succeeded
                        : ai::AIGuardFeedbackKind::Failed;
                    break;
                }

                const bool accepted =
                    m_world.m_objectSimulation.requestContainment(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Attach,
                         .container = command.target,
                         .object = command.correlation.subject,
                         .confirmedTick = m_presentation.m_confirmedTick,
                         .force = false}, &m_content.m_players, &m_content.m_contentSnapshot);
                sideEffectCommitted = accepted;
                const ObjectContainedByComponent* attached =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *subjectEntity);
                if (attached && attached->container == command.target) {
                    feedback.kind = ai::AIGuardFeedbackKind::Succeeded;
                } else if (accepted) {
                    // RailedTransportDock can accept an Attach while retaining
                    // ownership of the pull-inside phase. Keep this exact
                    // correlated command until the owner publishes the edge.
                    feedback.kind = ai::AIGuardFeedbackKind::Progress;
                    terminal = false;
                } else {
                    feedback.kind = ai::AIGuardFeedbackKind::Failed;
                }
                break;
            }
            case ai::AIGuardInteractionCommandKind::ExitTunnel: {
                if (!subjectEntity) {
                    feedback.kind = ai::AIGuardFeedbackKind::Failed;
                    break;
                }
                const ObjectContainedByComponent* existing =
                    ecs::try_get<ObjectContainedByComponent>(
                        m_world.m_registry, *subjectEntity);
                if (!existing) {
                    // A repeated detached ExitTunnel is already complete.
                    feedback.kind = ai::AIGuardFeedbackKind::Succeeded;
                    break;
                }
                const std::optional<ecs::entity> sourceEntity =
                    m_world.m_objects.entityFromId(existing->container);
                const std::optional<ecs::entity> targetEntity =
                    m_world.m_objects.entityFromId(command.target);
                const ObjectContainmentRuntimeComponent* sourceRuntime =
                    sourceEntity
                    ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                          m_world.m_registry, *sourceEntity)
                    : nullptr;
                const ObjectContainmentRuntimeComponent* targetRuntime =
                    targetEntity
                    ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                          m_world.m_registry, *targetEntity)
                    : nullptr;
                const bool sourceIsTunnel = sourceRuntime &&
                    sourceRuntime->plan &&
                    existing->containmentRuleIndex <
                        sourceRuntime->plan->rules.size() &&
                    sourceRuntime->plan
                            ->rules[existing->containmentRuleIndex]
                            .kind == ObjectContainmentKind::Tunnel;
                const bool targetIsTunnel = targetRuntime &&
                    targetRuntime->plan &&
                    std::any_of(
                        targetRuntime->plan->rules.begin(),
                        targetRuntime->plan->rules.end(),
                        [](const ObjectContainmentRule& rule) noexcept {
                            return rule.kind ==
                                ObjectContainmentKind::Tunnel;
                        });
                if (!sourceIsTunnel || !targetIsTunnel) {
                    feedback.kind = ai::AIGuardFeedbackKind::Failed;
                    break;
                }
                const bool accepted =
                    m_world.m_objectSimulation.requestContainment(
                        m_world.m_registry, m_world.m_objects,
                        {.kind = ObjectContainmentRequestKind::Detach,
                         .container = existing->container,
                         .object = command.correlation.subject,
                         .confirmedTick = m_presentation.m_confirmedTick,
                         .force = command.urgent}, &m_content.m_players,
                        &m_content.m_contentSnapshot);
                sideEffectCommitted = accepted;
                feedback.kind = accepted
                    ? ai::AIGuardFeedbackKind::Succeeded
                    : ai::AIGuardFeedbackKind::Failed;
                break;
            }
            case ai::AIGuardInteractionCommandKind::BeginPickUpCrate:
                // Production Guard pickup is consumed by the in-runtime
                // PickUpCrate child. A stale externally staged value degrades
                // to a normal child failure instead of making Guard itself
                // unsupported.
                feedback.kind = ai::AIGuardFeedbackKind::Failed;
                break;
            case ai::AIGuardInteractionCommandKind::Cancel:
            case ai::AIGuardInteractionCommandKind::EndGuard:
                break;
            }
        }
        const ai::ObjectAITransientStatus staged = transients.stage(feedback);
        if (staged == ai::ObjectAITransientStatus::Success &&
            terminal) {
            consumedInteraction.push_back(command);
        } else if (staged != ai::ObjectAITransientStatus::Success &&
                   sideEffectCommitted) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Feedback,
                .code = staged == ai::ObjectAITransientStatus::CapacityExceeded
                    ? SimulationFaultCode::CapacityExceeded
                    : SimulationFaultCode::InvalidEvent,
                .confirmedTick = m_presentation.m_confirmedTick,
                .subject = command.correlation.subject.value,
            }));
            return;
        }
    }
    for (const ai::AIGuardInteractionCommand& command :
         consumedInteraction) {
        static_cast<void>(transients.removeGuardInteractionCommand(
            command.correlation, command.kind));
    }
}

} // namespace engine
