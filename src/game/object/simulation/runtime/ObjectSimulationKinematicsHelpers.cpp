#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace engine {

using namespace object_simulation_detail;

void ObjectSimulation::updateKinematicsFloats(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick) {
        // FloatUpdate is deliberately not a force/buoyancy integration. It
        // snaps the post-writer position to the legacy water surface before
        // HeightDie evaluates its own terrain-relative threshold.
        object_simulation_detail::state(*this).m_float.update(registry, lifecycle, terrain, confirmedTick);
}

void ObjectSimulation::updateKinematicsContainment(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
        // Consume normal capture evacuation through the same high-level
        // request transaction used by commands.  Stable ObjectId order makes
        // the queue deterministic.  A zero ExitDelay may release the whole
        // queue in one tick; a non-zero delay admits one passenger and leaves
        // the remainder pending until the door becomes available.
        struct PendingEvacuation final {
            ObjectId container = INVALID_OBJECT_ID;
            ecs::entity entity = ecs::null;
        };
        container::Vector<PendingEvacuation> pendingEvacuations;
        const auto evacuationView = ecs::view<
            const ObjectIdentityComponent,
            ObjectContainmentRuntimeComponent,
            const ObjectContainmentComponent>(registry);
        for (const ecs::entity entity : evacuationView) {
            const ObjectIdentityComponent& identity =
                evacuationView.template get<
                    const ObjectIdentityComponent>(entity);
            const ObjectContainmentRuntimeComponent& runtime =
                evacuationView.template get<
                    ObjectContainmentRuntimeComponent>(entity);
            if (identity.id && runtime.ownerChangeEvacuationPending)
                pendingEvacuations.push_back({identity.id, entity});
        }
        std::sort(pendingEvacuations.begin(), pendingEvacuations.end(),
                  [](const PendingEvacuation& left,
                     const PendingEvacuation& right) {
                      return left.container < right.container;
                  });
        for (const PendingEvacuation& pending : pendingEvacuations) {
            ObjectContainmentRuntimeComponent* runtime =
                ecs::try_get<ObjectContainmentRuntimeComponent>(
                    registry, pending.entity);
            if (!runtime || !runtime->plan) continue;
            for (;;) {
                const ObjectContainmentComponent* contents =
                    ecs::try_get<ObjectContainmentComponent>(
                        registry, pending.entity);
                ObjectId nextPassenger = INVALID_OBJECT_ID;
                if (contents) {
                    for (const ObjectContainedObjectRecord& record :
                         contents->objects) {
                        const std::optional<ecs::entity> passenger =
                            lifecycle.entityFromIdIncludingPending(
                                record.object);
                        const ObjectContainedByComponent* edge = passenger
                            ? ecs::try_get<ObjectContainedByComponent>(
                                  registry, *passenger)
                            : nullptr;
                        if (!edge || edge->container != pending.container ||
                            edge->containmentRuleIndex >=
                                runtime->plan->rules.size()) {
                            continue;
                        }
                        const ObjectContainmentKind kind =
                            runtime->plan->rules[
                                edge->containmentRuleIndex].kind;
                        if (kind == ObjectContainmentKind::Transport ||
                            kind == ObjectContainmentKind::RiderChange) {
                            nextPassenger = record.object;
                            break;
                        }
                    }
                }
                if (!nextPassenger) {
                    runtime->ownerChangeEvacuationPending = false;
                    break;
                }
                const bool accepted = requestContainment(
                    registry, lifecycle,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = pending.container,
                     .object = nextPassenger,
                     .confirmedTick = confirmedTick},
                    context.players, context.content);
                if (!accepted) break;
            }
        }

        auto& containmentDamage =
            object_simulation_detail::state(*this).m_damageScratch;
        containmentDamage.clear();
        container::Vector<ObjectBodyStateProjection> bodyStateProjections;
        object_simulation_detail::state(*this).m_containment.update(registry, lifecycle, object_simulation_detail::state(*this).m_rules, confirmedTick,
                             object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
                             containmentDamage, bodyStateProjections,
                             &object_simulation_detail::state(*this).m_containmentEvents,
                             &object_simulation_detail::state(*this).m_transportEvents,
                             context.players,
                             &terrain, context.navigation);
        for (const ObjectBodyStateProjection& projection :
             bodyStateProjections) {
            static_cast<void>(applyBodyStateProjection(
                registry, lifecycle, projection));
        }
        if (context.content) {
            const auto parachutes = ecs::view<
                ObjectContainmentRuntimeComponent,
                const ThingTemplateComponent,
                ObjectLocomotionComponent>(registry);
            for (const ecs::entity entity : parachutes) {
                ObjectContainmentRuntimeComponent& runtime =
                    parachutes.template get<
                        ObjectContainmentRuntimeComponent>(entity);
                if (!runtime.parachuteOpened ||
                    runtime.parachuteOpenLocomotorProjected ||
                    !runtime.plan ||
                    std::none_of(runtime.plan->rules.begin(),
                                 runtime.plan->rules.end(),
                                 [](const ObjectContainmentRule& rule) {
                                     return rule.kind ==
                                         ObjectContainmentKind::Parachute;
                                 })) {
                    continue;
                }
                const ThingTemplateComponent& type =
                    parachutes.template get<
                        const ThingTemplateComponent>(entity);
                ObjectLocomotionComponent& locomotion =
                    parachutes.template get<ObjectLocomotionComponent>(
                        entity);
                if (!type.archetype) continue;
                container::Vector<game::FrozenLocomotorTemplate> selected =
                    collectRuntimeLocomotors(
                        type.archetype->templateData, *context.content,
                        game::LocomotorSetSlot::Normal);
                if (selected.empty()) continue;
                locomotion.profiles = std::move(selected);
                applyLocomotorTemplate(locomotion,
                                       locomotion.profiles.front());
                if (ObjectPhysicsComponent* physics =
                        ecs::try_get<ObjectPhysicsComponent>(registry,
                                                             entity)) {
                    physics->allowToFall = false;
                    physics->inFreeFall = false;
                }
                runtime.parachuteOpenLocomotorProjected = true;
            }
        }
        for (ObjectDamageRequest& request : containmentDamage) {
            queueDamage(std::move(request));
        }
}

void ObjectSimulation::queueKinematicsHeightDeaths(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick) {
        container::Vector<ObjectHeightDieCommand> commands;
        container::Vector<ObjectHeightDiePresentationEvent> presentation;
        object_simulation_detail::state(*this).m_heightDie.update(registry, lifecycle, terrain, object_simulation_detail::state(*this).m_rules, confirmedTick,
                           commands, presentation);
        object_simulation_detail::state(*this).m_heightDiePresentationEvents.insert(object_simulation_detail::state(*this).m_heightDiePresentationEvents.end(),
                                             std::make_move_iterator(presentation.begin()),
                                             std::make_move_iterator(presentation.end()));
        for (const ObjectHeightDieCommand& command : commands) {
            // Object::kill() is a force-kill Body transaction, never a direct
            // lifecycle deletion. Preserve the one source-invalid command per
            // HeightDie occurrence and let the shared Die chain decide what
            // remains after it dies.
            queueDamage({
                .target = command.object,
                .sourceSequence = command.authoredOrder,
                .causalGroup = command.object,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::NORMAL,
                .forceKill = true,
                .confirmedTick = confirmedTick,
            });
        }
}

void ObjectSimulation::finishKinematicsCrateCollisions(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
        if (!context.players || !context.content) return;
        const size_t commandBegin = object_simulation_detail::state(*this)
            .m_cratePickupCommands.size();
        object_simulation_detail::state(*this).m_crateCollide.update(registry, lifecycle,
                              object_simulation_detail::state(*this).m_contactIndex,
                              terrain, *context.players,
                              *context.content, object_simulation_detail::state(*this).m_rules, confirmedTick,
                              object_simulation_detail::state(*this).m_cratePickupCommands);
        for (size_t index = commandBegin;
             index < object_simulation_detail::state(*this)
                         .m_cratePickupCommands.size(); ++index) {
            object_simulation_detail::state(*this)
                .m_cratePickupCommands[index].submissionOrdinal =
                    reserveGameplaySubmissionOrdinal();
        }
}

void ObjectSimulation::finishKinematicsFireWeaponCollisions(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
        if (!context.content || !context.random) return;
        object_simulation_detail::state(*this).m_fireWeaponCollide.update(
            registry, lifecycle, object_simulation_detail::state(*this).m_contactIndex,
            *context.content, *context.random,
            confirmedTick, object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
            object_simulation_detail::state(*this).m_systemWeaponFireCommands);
}

void ObjectSimulation::updateSquishCollisionPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
        if (!context.players) return;
        auto& squishDamage =
            object_simulation_detail::state(*this).m_damageScratch;
        squishDamage.clear();
        object_simulation_detail::state(*this).m_squishCollide.update(registry, lifecycle,
                               object_simulation_detail::state(*this).m_contactIndex,
                               terrain, *context.players,
                               object_simulation_detail::state(*this).m_rules, confirmedTick, squishDamage);
        for (ObjectDamageRequest& request : squishDamage) {
            queueDamage(std::move(request));
        }
}

void ObjectSimulation::updateKinematicsAnimationSteering(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
        object_simulation_detail::state(*this).m_animationSteering.update(registry, lifecycle, object_simulation_detail::state(*this).m_rules,
                                   confirmedTick);
}

void ObjectSimulation::prepareKinematicsCollisionPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectUpgradeExecutionContext context) {
        if (context.players || (context.content && context.random)) {
            object_simulation_detail::state(*this).m_contactIndex.rebuild(
                registry, lifecycle);
        }
}

void ObjectSimulation::updateBridgeRailPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick) {
        // Railroad owns its final track-constrained pose, while the ferry AI
        // publishes only a value Move intent for the next locomotion step.
        auto& railroadDamage =
            object_simulation_detail::state(*this).m_damageScratch;
        railroadDamage.clear();
        object_simulation_detail::state(*this).m_bridge.update(registry, lifecycle, terrain, object_simulation_detail::state(*this).m_rules,
                        confirmedTick, object_simulation_detail::state(*this).m_bridgeStateEvents,
                        object_simulation_detail::state(*this)
                            .m_nextGameplaySubmissionOrdinal,
                        object_simulation_detail::state(*this)
                            .m_railedTransportDockAttachCompletions,
                        object_simulation_detail::state(*this).m_railroadCarriageSpawnRequests,
                        object_simulation_detail::state(*this)
                            .m_railroadDisembarkRequests,
                        railroadDamage,
                        object_simulation_detail::state(*this)
                            .m_railroadPresentationEvents);
        for (ObjectDamageRequest& request : railroadDamage) {
            queueDamage(std::move(request));
        }
}

bool ObjectSimulation::commitRailedTransportDockAttachCompletion(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectRailedTransportDockAttachCompletion& completion) {
    auto& simulationState = object_simulation_detail::state(*this);
    bool accepted = completion.accepted;
    if (accepted) {
        accepted = simulationState.m_containment.commitPreparedAttach(
            registry, lifecycle,
            {.container = completion.request.container,
             .object = completion.request.object,
             .containmentRuleIndex =
                 completion.request.containmentRuleIndex,
             .destroyWithContainer =
                 completion.request.destroyWithContainer,
             .enclosing = completion.request.enclosing,
             .followsContainerTransform =
                 completion.request.followsContainerTransform},
            completion.request.confirmedTick,
            simulationState.m_containmentEvents);
    } else {
        simulationState.m_containmentEvents.push_back({
            .kind = ObjectContainmentRequestKind::Attach,
            .container = completion.request.container,
            .object = completion.request.object,
            .confirmedTick = completion.request.confirmedTick,
            .accepted = false,
        });
    }
    return accepted;
}

void ObjectSimulation::acknowledgeRailedTransportDockAttachCompletion(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectRailedTransportDockAttachCompletion& completion,
    bool accepted) {
    object_simulation_detail::state(*this)
        .m_bridge.acknowledgeRailedTransportDockAttach(
            registry, lifecycle, completion.request.container,
            completion.request.object, completion.request.dockRuleIndex,
            accepted, completion.request.confirmedTick);
}

bool ObjectSimulation::executeRailroadDisembark(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectRailroadDisembarkRequest& request) {
    auto& simulationState = object_simulation_detail::state(*this);
    static_cast<void>(simulationState.m_containment.requestEjectAll(
        registry, lifecycle,
        {.kind = ObjectContainmentRequestKind::EjectAll,
         .container = request.carriage,
         .confirmedTick = request.confirmedTick},
        simulationState.m_containmentEvents));
    return true;
}

void ObjectSimulation::updateSpawnSlavePhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
        // Veterancy projection below may enter upgrade effect callbacks before
        // these requests are queued. Keep this buffer local so a future
        // callback cannot clear the serialized phase scratch out from under
        // the still-live SpawnSlave result.
        container::Vector<ObjectDamageRequest> slaveDamage;
        container::Vector<ObjectSpawnVeterancyRequest> veterancyRequests;
        container::Vector<ObjectBodyHealthProjection>
            bodyHealthProjections;
        object_simulation_detail::state(*this).m_spawnSlave.update(
            registry, lifecycle, context.players, context.content,
            context.spatialIndex, &terrain, context.random, object_simulation_detail::state(*this).m_rules,
            confirmedTick,
            object_simulation_detail::state(*this)
                .m_nextGameplaySubmissionOrdinal,
            object_simulation_detail::state(*this).m_spawnSlaveRequests, slaveDamage, veterancyRequests,
            bodyHealthProjections,
            object_simulation_detail::state(*this).m_deleteDestroyRequests,
            object_simulation_detail::state(*this).m_objectDefectionRequests,
            object_simulation_detail::state(*this)
                .m_slaveRepairPresentationEvents,
            object_simulation_detail::state(*this)
                .m_tensileNavigationEvents,
            object_simulation_detail::state(*this)
                .m_tensileFormationEvents);
        std::sort(veterancyRequests.begin(), veterancyRequests.end(),
            [](const ObjectSpawnVeterancyRequest& left,
               const ObjectSpawnVeterancyRequest& right) {
                if (left.object != right.object)
                    return left.object < right.object;
                return static_cast<uint8_t>(left.level) >
                    static_cast<uint8_t>(right.level);
            });
        ObjectId lastVeterancyObject = INVALID_OBJECT_ID;
        for (const ObjectSpawnVeterancyRequest& request :
             veterancyRequests) {
            if (request.object == lastVeterancyObject) continue;
            lastVeterancyObject = request.object;
            const std::optional<ecs::entity> entity =
                lifecycle.entityFromId(request.object);
            const OwnerComponent* owner = entity
                ? ecs::try_get<OwnerComponent>(registry, *entity) : nullptr;
            const PlayerState* player = owner && context.players
                ? context.players->get(owner->player) : nullptr;
            const UpgradeMask upgrades = player
                ? player->upgrades.completed
                : UpgradeMask{};
            if (entity) {
                static_cast<void>(setObjectVeterancyLevel(
                    registry, lifecycle, request.object, request.level,
                    upgrades, confirmedTick, context));
            }
        }
        std::sort(bodyHealthProjections.begin(),
                  bodyHealthProjections.end(),
            [](const ObjectBodyHealthProjection& left,
               const ObjectBodyHealthProjection& right) {
                return left.object < right.object;
            });
        for (const ObjectBodyHealthProjection& projection :
             bodyHealthProjections) {
            static_cast<void>(applyBodyHealthProjection(
                registry, lifecycle, projection));
        }
        for (ObjectDamageRequest& request : slaveDamage)
            queueDamage(std::move(request));
}

void ObjectSimulation::updateKinematicsSmartBombs(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick) {
        object_simulation_detail::state(*this).m_smartBomb.update(registry, lifecycle, terrain, object_simulation_detail::state(*this).m_rules,
                           confirmedTick);
}

void ObjectSimulation::updateKinematicsStickyBombs(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick) {
        object_simulation_detail::state(*this).m_stickyBomb.update(
            registry, lifecycle, terrain, object_simulation_detail::state(*this).m_rules, confirmedTick,
            object_simulation_detail::state(*this).m_stickyBombPresentationEvents);
}

void ObjectSimulation::updateWaveGuideKinematicsPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
        if (!context.random) return;
        auto& damage =
            object_simulation_detail::state(*this).m_damageScratch;
        damage.clear();
        object_simulation_detail::state(*this).m_waveGuide.update(
            registry, lifecycle, terrain, *context.random, object_simulation_detail::state(*this).m_rules,
            confirmedTick, object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal, damage,
            object_simulation_detail::state(*this).m_waveGuideBridgeImpacts,
            object_simulation_detail::state(*this).m_waveGuideEvents, object_simulation_detail::state(*this).m_objectFireAudioCommands);
        for (ObjectDamageRequest& request : damage) {
            queueDamage(std::move(request));
        }
}

} // namespace engine
