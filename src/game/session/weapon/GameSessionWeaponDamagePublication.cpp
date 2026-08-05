#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/frame/GameSessionTransitionFxProjection.h"

#include "debug/debug.h"
#include "core/container/string_utils.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

namespace engine::detail {

bool GameSessionWeaponEventDrain::processDamage(WorkItem item) {
    if (++m_processedDamage > kMaximumSystemWeaponDamageRequests) {
        TD_LOG_ERROR(
            "[GameSession] System weapon reaction chain exceeded {} damage requests at tick {}; dropping the malformed tail",
            kMaximumSystemWeaponDamageRequests, m_presentation.m_confirmedTick);
        discardPendingWork();
        return false;
    }
    // Reserve the complete possible onDie structure before Body mutates HP:
    // initial DeathWalk + one continuation per authored Behavior + BodyResume.
    // A non-lethal transaction releases the reservation immediately.
    size_t structuralReservation = 2u;
    const std::optional<ecs::entity> prospectiveVictim =
        m_world.m_objects.entityFromId(
            item.damage.target);
    const ObjectDeathReactionComponent* prospectiveReactions =
        prospectiveVictim
            ? ecs::try_get<ObjectDeathReactionComponent>(
                  m_world.m_registry,
                  *prospectiveVictim)
            : nullptr;
    if (prospectiveReactions && prospectiveReactions->plan) {
        const size_t behaviorCount =
            prospectiveReactions->plan->onDieBehaviors.size();
        if (behaviorCount >
            kMaximumGameplayTransactions - structuralReservation) {
            structuralReservation = kMaximumGameplayTransactions + 1u;
        } else {
            structuralReservation += behaviorCount;
        }
    }
    if (structuralReservation > kMaximumGameplayTransactions ||
        m_processedTransactions + m_reservedStructuralTransactions >
            kMaximumGameplayTransactions - structuralReservation) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::ObjectSimulation,
            .code = SimulationFaultCode::CapacityExceeded,
            .confirmedTick = item.damage.confirmedTick,
            .subject = item.damage.target.value,
            .sequence = item.damage.sourceSequence,
        }));
        discardPendingWork();
        m_work.clear();
        return true;
    }
    m_reservedStructuralTransactions += structuralReservation;
    const size_t floor = m_work.size();
    ++m_damageResolutionDepth;
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    ObjectDamageTransactionResult transactionResult =
        simulation.executeDamageTransaction(
            m_world.m_registry,
            m_world.m_objects,
            std::move(item.damage),
            m_presentation.m_confirmedTick,
            {.players = &m_content.m_players,
             .scienceCatalog = m_content
                                   .m_contentSnapshot.scienceCatalog(),
             .content =
                 &m_content.m_contentSnapshot,
             .random =
                 &m_content.m_simulationRandom,
              .terrain = &m_content.m_terrain,
              .effects = &simulation});
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    if (transactionResult.deathWalk || transactionResult.bodyResume) {
        if (!transactionResult.deathWalk || !transactionResult.bodyResume) {
            m_reservedStructuralTransactions -= structuralReservation;
            return false;
        }
        // Reserve the Body continuation below the onDie walk. Body-produced
        // onDamage children are pushed last and therefore close first.
        pushWork({
            .kind = WorkKind::BodyResume,
            .bodyResume = std::move(*transactionResult.bodyResume),
        });
        pushWork({
            .kind = WorkKind::DeathWalk,
            .deathWalk = std::move(*transactionResult.deathWalk),
        });
    } else {
        m_reservedStructuralTransactions -= structuralReservation;
    }
    pushPendingWork(std::move(nested));
    drainToSize(floor);

    --m_damageResolutionDepth;
    if (m_frame.result().faulted()) {
        // The central drain already discarded the remaining causal stack.
        // Do not project a partially closed Body journal into score, AI,
        // audio or frame events after the fail-stop boundary.
        return true;
    }
    // Re-entrant handler closure shares ObjectSimulation's health journal
    // with its caller. Publishing at an inner depth would consume the outer
    // lethal hit before its authored onDie walk appended Died.
    if (m_damageResolutionDepth == 0) {
        m_healthEvents.consume();
    }
    return true;
}

void GameSessionWeaponEventDrain::processUpgradeFx(const WorkItem& item) {
    const ObjectUpgradeFxInvocation& fx = item.upgradeFx;
    if (!fx.fxList.empty()) {
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .fxListName = fx.fxList,
            .anchorKind =
                game::FxInvocationAnchorKind::ObjectAttachment,
            .primary = {
                .object = fx.source,
                .position = {
                    fx.position.x.to_float(),
                    fx.position.y.to_float(),
                    fx.position.z.to_float(),
                },
                .rollRadians = fx.rollRadians.to_float(),
                .pitchRadians = fx.pitchRadians.to_float(),
                .yawRadians = fx.yawRadians.to_float(),
            },
        }));
    }
}

void GameSessionWeaponEventDrain::processStructureFx(const WorkItem& item) {
    const ObjectStructureEffectEvent& fx = item.structureFx;
    const bool attached = fx.anchor ==
        ObjectStructureEffectAnchor::ObjectAttachment;
    static_cast<void>(m_publication.emitFxInvocationEvent({
        .fxListName = fx.resource,
        .anchorKind = attached
            ? game::FxInvocationAnchorKind::ObjectAttachment
            : game::FxInvocationAnchorKind::WorldPosition,
        .primary = {
            .object = attached ? fx.object : INVALID_OBJECT_ID,
            .position = {
                fx.position.x.to_float(),
                fx.position.y.to_float(),
                fx.position.z.to_float(),
            },
            .yawRadians = fx.orientationRadians.to_float(),
        },
    }));
}

void GameSessionWeaponEventDrain::appendDeathPayloadWork(
    container::Vector<WorkItem>& output, ObjectId object,
    const std::optional<container::String>& ocl,
    const std::optional<container::String>& weapon,
    uint32_t sourceSequence, uint32_t authoredOrder,
    uint64_t sourceEmissionSequence, uint64_t confirmedTick,
    std::optional<LogicFixedVec3> frozenPosition,
    std::optional<ObjectPhysicsComponent::Scalar> frozenRotation,
    PlayerId frozenOwner, uint32_t frozenSourcePathfindLayer) {
    const std::optional<ecs::entity> entity =
        m_world.m_objects
            .entityFromIdIncludingPending(object);
    if (!entity) return;
    const OwnerComponent* ownerComponent =
        ecs::try_get<OwnerComponent>(
            m_world.m_registry, *entity);
    const PrimaryTeamComponent* teamComponent =
        ecs::try_get<PrimaryTeamComponent>(
            m_world.m_registry, *entity);
    const PlayerId owner = frozenOwner
        ? frozenOwner
        : ownerComponent ? ownerComponent->player : INVALID_PLAYER_ID;
    const ObjectTeamId team = teamComponent
        ? teamComponent->team
        : m_world.m_objectTeams
              .defaultTeam(owner)
              .value_or(INVALID_OBJECT_TEAM_ID);
    if (!owner || !team) return;

    const TransformComponent* transform = ecs::try_get<TransformComponent>(
        m_world.m_registry, *entity);
    LogicFixedVec3 position = frozenPosition.value_or(
        transform
            ? readAuthoritativeObjectPosition(
                  m_world.m_registry, *entity,
                  *transform)
            : LogicFixedVec3{});
    ObjectPhysicsComponent::Scalar rotation = frozenRotation.value_or(
        transform
            ? readAuthoritativeObjectYaw(
                  m_world.m_registry, *entity,
                  *transform)
            : ObjectPhysicsComponent::Scalar{});
    ObjectPhysicsComponent::Scalar pitch{};
    ObjectPhysicsComponent::Scalar roll{};
    bool ownsFullAttitude = false;
    LogicFixedVec3 velocity;
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(
                m_world.m_registry, *entity)) {
        velocity = physics->velocityUnitsPerSecond;
        if (!frozenRotation && physics->ownsAttitude) {
            rotation = physics->yaw;
            pitch = physics->pitch;
            roll = physics->roll;
            ownsFullAttitude = true;
        }
    }
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    if (const ObjectVeterancyComponent* state =
            ecs::try_get<ObjectVeterancyComponent>(
                m_world.m_registry, *entity)) {
        veterancy = state->level;
    }
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(
            m_world.m_registry, *entity);
    const uint64_t payloadBase =
        sourceEmissionSequence <=
                (std::numeric_limits<uint64_t>::max() - 3u) / 4u
            ? sourceEmissionSequence * 4u
            : sourceEmissionSequence;

    if (ocl && !ocl->empty()) {
        const game::ObjectCreationListContentId content =
            m_content.m_contentSnapshot
                .findObjectCreationListId(*ocl);
        if (content) {
            output.push_back({
                .kind = WorkKind::ObjectCreationList,
                .ocl = {
                    .content = content,
                    .source = object,
                    .owner = owner,
                    .primaryTeam = team,
                    .primaryPosition = position,
                    .sourceVelocity = velocity,
                    .orientationRadians = rotation,
                    .pitchRadians = pitch,
                    .rollRadians = roll,
                    .veterancy = veterancy,
                    .authoredOrder = authoredOrder,
                    .emissionSequence = payloadBase + 1u,
                    .confirmedTick = confirmedTick,
                    .sourcePathfindLayer = frozenSourcePathfindLayer,
                    .sourceAirborne = airborne && airborne->isAirborne,
                    .sourceOwnsFullAttitude = ownsFullAttitude,
                },
                .oclState = std::make_shared<OclWorkState>(),
            });
        }
    }
    if (weapon && !weapon->empty()) {
        const game::WeaponContentId content =
            m_content.m_contentSnapshot
                .findWeaponId(*weapon);
        container::Vector<ObjectSystemWeaponFireCommand> commands;
        if (content && queueObjectTransientWeaponFire(
                content, m_world.m_registry,
                *entity, object,
                m_content.m_contentSnapshot,
                m_content.m_simulationRandom,
                sourceSequence, authoredOrder, payloadBase + 2u,
                confirmedTick, commands)) {
            for (ObjectSystemWeaponFireCommand& command : commands) {
                output.push_back({
                    .kind = WorkKind::Weapon,
                    .weapon = std::move(command),
                });
            }
        }
    }
}

bool GameSessionWeaponEventDrain::processTransitionOcl(
    const WorkItem& item) {
    const ObjectTransitionDamageFxEvent& source = item.transitionOcl;
    if (source.resource.empty() ||
        (source.oclRequiresDamageSource && !source.hasSecondary)) {
        return true;
    }
    const game::ObjectCreationListContentId content =
        m_content.m_contentSnapshot
            .findObjectCreationListId(source.resource);
    const std::optional<ecs::entity> entity =
        m_world.m_objects
            .entityFromIdIncludingPending(source.object);
    if (!content || !entity) return true;
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *entity);
    const PrimaryTeamComponent* team =
        ecs::try_get<PrimaryTeamComponent>(
            m_world.m_registry, *entity);
    if (!owner || !team || !owner->player || !team->team) return true;
    LogicFixedVec3 velocity;
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(
                m_world.m_registry, *entity)) {
        velocity = physics->velocityUnitsPerSecond;
    }
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    if (const ObjectVeterancyComponent* state =
            ecs::try_get<ObjectVeterancyComponent>(
                m_world.m_registry, *entity)) {
        veterancy = state->level;
    }
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(
            m_world.m_registry, *entity);
    container::Vector<WorkItem> nested;
    nested.push_back({
        .kind = WorkKind::ObjectCreationList,
        .ocl = {
            .content = content,
            .source = source.object,
            .owner = owner->player,
            .primaryTeam = team->team,
            .primaryPosition =
                session_fx::transitionWorldPositionFixed(
                    source),
            .secondaryPosition = source.secondary.position,
            .sourceVelocity = velocity,
            .orientationRadians = {},
            .veterancy = veterancy,
            .authoredOrder = source.authoredOrder,
            .emissionSequence = source.emissionSequence,
            .confirmedTick = source.confirmedTick,
            .sourcePathfindLayer = source.sourcePathfindLayer,
            .hasSecondaryPosition = source.hasSecondary,
            .sourceAirborne = airborne && airborne->isAirborne,
        },
        .oclState = std::make_shared<OclWorkState>(),
    });
    pushPendingWork(std::move(nested));
    return true;
}

bool GameSessionWeaponEventDrain::processInstantDeath(
    const WorkItem& item) {
    const ObjectInstantDeathEffectEvent& source = item.instantDeath;
    container::Vector<WorkItem> nested;
    appendDeathPayloadWork(
        nested, source.object, source.ocl, source.weapon,
        source.sourceSequence, source.authoredOrder,
        source.fxEmissionSequence, source.confirmedTick,
        source.position, source.rotationRadians, source.owner,
        source.sourcePathfindLayer);
    pushPendingWork(std::move(nested));
    return true;
}

bool GameSessionWeaponEventDrain::processSlowDeath(
    const WorkItem& item) {
    const ObjectSlowDeathPhaseEvent& source = item.slowDeath;
    container::Vector<WorkItem> nested;
    appendDeathPayloadWork(
        nested, source.object, source.ocl, source.weapon,
        source.sourceSequence, source.authoredOrder,
        source.fxEmissionSequence, source.confirmedTick,
        std::nullopt, std::nullopt, INVALID_PLAYER_ID,
        source.sourcePathfindLayer);
    if (source.rubbleObject && !source.rubbleObject->empty() &&
        source.hasRubbleSpawnState) {
        const uint64_t payloadBase =
            source.fxEmissionSequence <=
                    (std::numeric_limits<uint64_t>::max() - 3u) / 4u
                ? source.fxEmissionSequence * 4u
                : source.fxEmissionSequence;
        nested.push_back({
            .kind = WorkKind::SlowDeathRubble,
            .slowDeathRubble = {
                .source = source.object,
                .owner = source.rubbleOwner,
                .primaryTeam = source.rubblePrimaryTeam,
                .objectTemplate = *source.rubbleObject,
                .transform = source.rubbleTransform,
                .sourcePathfindLayer = source.sourcePathfindLayer,
                .authoredOrder = source.authoredOrder,
                .emissionSequence = payloadBase + 3u,
                .confirmedTick = source.confirmedTick,
            },
        });
    }
    pushPendingWork(std::move(nested));
    return true;
}

void GameSessionWeaponEventDrain::processSlowDeathRubble(
    const WorkItem& item) {
    const SlowDeathRubbleWork& rubble = item.slowDeathRubble;
    const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject({
        .templateName = rubble.objectTemplate,
        .owner = rubble.owner,
        .primaryTeam = rubble.primaryTeam,
        .transform = rubble.transform,
        .initialPathfindLayer = rubble.sourcePathfindLayer,
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = rubble.confirmedTick,
        .producer = rubble.source,
    });
    if (!spawned) return;
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
}

void GameSessionWeaponEventDrain::processTopplePathfind(
    const WorkItem& item) {
    const ObjectTopplePathfindRemovalRequest& request =
        item.topplePathfind;
    static_cast<void>(m_navigation.submitBuildingState(
        request.object, request.confirmedTick,
        navigation::NavigationDynamicEventReason::BuildingDestroyed,
        navigation::NavigationBuildingState::Absent, false));
}

void GameSessionWeaponEventDrain::processToppleStump(
    const WorkItem& item) {
    const ObjectToppleStumpSpawnRequest& request = item.toppleStump;
    if (request.confirmedTick !=
            m_presentation.m_confirmedTick ||
        request.objectTemplate.empty() ||
        !m_content.m_contentSnapshot
             .findObjectArchetype(request.objectTemplate)) {
        return;
    }
    const GameSessionObjectSpawnResult stump = m_lifecycle.spawnObject({
        .templateName = request.objectTemplate,
        .owner = NEUTRAL_PLAYER_ID,
        .transform = ObjectFixedTransformComponent{
            .position = request.position,
            .yawRadians = request.yawRadians,
            .authoritative = true,
        },
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = request.confirmedTick,
        .producer = request.source,
    });
    if (!stump.entity) return;
    ecs::emplace<ObjectToppleStumpOwnerComponent>(
        m_world.m_registry, *stump.entity,
        ObjectToppleStumpOwnerComponent{
            .source = request.source,
            .ruleIndex = request.ruleIndex,
        });
    if (request.burned) {
        const game::ModelConditionMask burned =
            game::modelConditionMaskOf(game::ModelConditionFlag::Burned);
        publishObjectModelConditionContribution(
            m_world.m_registry, *stump.entity,
            ObjectModelConditionContributionSource::Tactical,
            {}, burned, request.confirmedTick,
            request.authoredOrder);
    }
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
}

void GameSessionWeaponEventDrain::processPhysicsCrash(
    const WorkItem& item) {
    const ObjectPhysicsCrashCommand& crash = item.physicsCrash;
    container::Vector<WorkItem> nested;
    const uint64_t sequence =
        m_world.m_objectSimulation
            .reserveGameplaySubmissionOrdinal();
    const std::optional<ecs::entity> source =
        m_world.m_objects
            .entityFromIdIncludingPending(crash.source);
    const std::optional<ecs::entity> target =
        m_world.m_objects
            .entityFromIdIncludingPending(crash.target);
    if (crash.weapon && source && target) {
        container::Vector<ObjectSystemWeaponFireCommand> commands;
        if (queueObjectTargetedTransientWeaponFire(
                crash.weapon,
                m_world.m_registry, *source,
                crash.source, *target, crash.target,
                m_content.m_contentSnapshot,
                m_content.m_simulationRandom,
                crash.sourceSequence, 0, sequence,
                crash.confirmedTick, commands)) {
            for (ObjectSystemWeaponFireCommand& command : commands) {
                nested.push_back({
                    .kind = WorkKind::Weapon,
                    .weapon = std::move(command),
                });
            }
        }
    }
    if (crash.destroySource) {
        nested.push_back({
            .kind = WorkKind::DestroyObject,
            .destroyObject = {
                .object = crash.source,
                .reason = ObjectDestroyReason::System,
                .source = crash.source,
                .emissionSequence = sequence == UINT64_MAX
                    ? UINT64_MAX : sequence + 1u,
                .confirmedTick = crash.confirmedTick,
            },
        });
    }
    // Weapon is ordered before DestroyObject. Any nested Damage/Die/OCL work
    // emitted by the weapon is pushed above the destroy continuation, which
    // reproduces PhysicsBehavior's synchronous fire-then-destroy call chain.
    pushPendingWork(std::move(nested));
}

void GameSessionWeaponEventDrain::processDestroyObject(
    const WorkItem& item) {
    const DestroyObjectWork& request = item.destroyObject;
    static_cast<void>(m_lifecycle.requestDestroyObject(
        request.object, request.reason, request.confirmedTick));
}

void GameSessionWeaponEventDrain::processMineSpawn(
    const WorkItem& item) {
    const ObjectMineSpawnCommand& command = item.mineSpawn;
    const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject({
        .templateName = command.templateName,
        .owner = command.owner,
        .primaryTeam = command.primaryTeam,
        .transform = ObjectFixedTransformComponent{
            .position = command.position,
            .yawRadians = command.yaw,
            .authoritative = true,
        },
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = command.confirmedTick,
        .producer = command.producer,
    });
    if (!spawned) return;
    if (spawned.entity) {
        ecs::emplace<ObjectGeneratedMineRecord>(
            m_world.m_registry,
            *spawned.entity,
            ObjectGeneratedMineRecord{
                .object = spawned.object,
                .generatorIndex = command.generatorIndex,
            });
    }
    static_cast<void>(
        m_world.m_objectSimulation
            .configureMineScoot(
                m_world.m_registry,
                m_world.m_objects,
                spawned.object, command.scootStart, command.position,
                command.confirmedTick));
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
}

void GameSessionWeaponEventDrain::processParticleUplinkRemnant(
    const WorkItem& item) {
    const ObjectParticleUplinkRemnantSpawnRequest& request =
        item.particleUplinkRemnant;
    if (request.confirmedTick !=
            m_presentation.m_confirmedTick ||
        request.objectTemplate.empty() ||
        !m_content.m_contentSnapshot
             .findObjectArchetype(request.objectTemplate)) {
        return;
    }
    const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject({
        .templateName = request.objectTemplate,
        .owner = request.owner,
        .primaryTeam = request.primaryTeam,
        .transform = ObjectFixedTransformComponent{
            .position = request.position,
            .authoritative = true,
        },
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = request.confirmedTick,
        .producer = request.source,
    });
    if (!spawned) return;
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
}

void GameSessionWeaponEventDrain::processWaveBridgeImpact(
    const WorkItem& item) {
    const ObjectWaveGuideBridgeImpact& impact = item.waveBridgeImpact;
    const GameSessionObjectSpawnResult replacement = m_lifecycle.spawnObject({
        .templateName = "WaterWaveBridge",
        .transform = ObjectFixedTransformComponent{
            .position = impact.position,
            .yawRadians = impact.targetRotationRadians,
            .authoritative = true,
        },
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = impact.confirmedTick,
        .producer = impact.source,
    });
    // ZH aborts deleteBridge when WaterWaveBridge allocation fails.
    if (!replacement) return;

    if (!impact.bridgeParticle.empty()) {
        static_cast<void>(m_publication.emitFxInvocationEvent({
            .directParticle = game::FxDirectParticleRequest{
                .particleSystemName = impact.bridgeParticle,
                .emitterCount = 1,
            },
            .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
            .primary = {
                .object = impact.source,
                .position = {impact.position.x.to_float(),
                             impact.position.y.to_float(),
                             impact.position.z.to_float()},
                .yawRadians =
                    impact.bridgeParticleRotationRadians.to_float(),
            },
        }));
    }
    if (!m_bridges.collapseTerrainSurface(
            impact.terrainSourceRecordIndex, impact.confirmedTick)) {
        return;
    }

    // Bridge collapse can synchronously resolve deaths for occupants.  Close
    // that authored reaction chain before retiring the bridge object itself,
    // matching the previous spawn -> terrain collapse -> destroy ordering.
    if (impact.target) {
        pushWork({
            .kind = WorkKind::DestroyObject,
            .destroyObject = {
                .object = impact.target,
                .reason = ObjectDestroyReason::System,
                .source = impact.source,
                .emissionSequence = impact.emissionSequence,
                .confirmedTick = impact.confirmedTick,
            },
        });
    }
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
}

} // namespace engine::detail
