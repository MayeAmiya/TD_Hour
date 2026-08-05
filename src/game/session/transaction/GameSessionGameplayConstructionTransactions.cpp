#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/presentation/GameSessionObjectAmbientAudioLifecycle.h"

#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/runtime/ObjectStatus.h"

#include <algorithm>

namespace engine::detail {

void GameSessionWeaponEventDrain::retargetStickyBombTargets(
    ObjectId from, ObjectId to) {
    if (!from || !to || from == to) return;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectStickyBombComponent>(
        m_world.m_registry);
    container::Vector<ObjectId> bombs;
    bombs.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectStickyBombComponent& component =
            view.template get<const ObjectStickyBombComponent>(entity);
        if (std::none_of(
                component.instances.begin(), component.instances.end(),
                [from](const ObjectStickyBombRuntime& runtime) {
                    return runtime.attached && !runtime.detonated &&
                        runtime.target == from;
                })) {
            continue;
        }
        bombs.push_back(view.template get<
            const ObjectIdentityComponent>(entity).id);
    }
    std::sort(bombs.begin(), bombs.end());
    for (const ObjectId bomb : bombs) {
        static_cast<void>(m_world
            .m_objectSimulation.retargetStickyBomb(
                m_world.m_registry,
                m_world.m_objects,
                bomb, to));
    }
}

bool GameSessionWeaponEventDrain::handleConstructionCompletion(
    WorkItem item) {
    const ObjectConstructionCompletionIntent& intent =
        item.constructionCompletion;
    if (intent.object &&
        intent.confirmedTick ==
            m_presentation.m_confirmedTick) {
        const ObjectId builder = intent.builder;
        if (!m_lifecycle.completeConstruction(
                intent.object, intent.confirmedTick)) {
            // Returning false aborts the complete confirmed causal stack and
            // publishes an InvalidEvent fault. Never consume the builder's
            // one-shot completion intent after a partial/failed transaction.
            return false;
        }
        // Units may enter the footprint during the build interval. Re-run the
        // same new-goal evacuation after the completed footprint is committed
        // so construction has a closed placement-to-completion lifecycle.
        static_cast<void>(m_lifecycle.evacuateConstructionFootprint(
            intent.object, builder, intent.confirmedTick));
        const PlayerState* localPlayer = m_content.m_players.localPlayer();
        const std::optional<ecs::entity> builderEntity = builder
            ? m_world.m_objects.entityFromId(builder) : std::nullopt;
        const ThingTemplateComponent* builderType = builderEntity
            ? ecs::try_get<ThingTemplateComponent>(
                  m_world.m_registry, *builderEntity)
            : nullptr;
        if (localPlayer && builderType && builderType->archetype &&
            m_world.m_ownership.ownerOf(builder) == localPlayer->id) {
            const game::ThingTemplate& templateData =
                builderType->archetype->templateData;
            const container::StringView cue =
                templateData.resolveUnitSound(
                    templateData.voiceTaskComplete,
                    "VoiceTaskComplete");
            if (!cue.empty()) {
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = container::String{cue},
                    .emitter = builder,
                    .owner = builder,
                }));
            }
        }
        const std::optional<ecs::entity> completedEntity =
            m_world.m_objects.entityFromId(intent.object);
        const ObjectHealthComponent* completedHealth = completedEntity
            ? ecs::try_get<ObjectHealthComponent>(
                  m_world.m_registry, *completedEntity)
            : nullptr;
        if (completedHealth && completedHealth->damageState !=
                ObjectBodyDamageState::Pristine) {
            GameSessionObjectAmbientAudioLifecycle{
                m_content, m_world, m_presentation, m_publication}
                .refresh(intent.object, completedHealth->damageState);
        }
        closeCurrentReaction();
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleBridgeRepairScaffoldBatch(
    WorkItem item) {
    container::Vector<ObjectBridgeRepairScaffoldIntent>& intents =
        item.bridgeRepairScaffoldBatch.intents;
    std::sort(
        intents.begin(), intents.end(),
        [](const ObjectBridgeRepairScaffoldIntent& left,
           const ObjectBridgeRepairScaffoldIntent& right) {
            if (left.bridge != right.bridge)
                return left.bridge < right.bridge;
            if (left.kind != right.kind) return left.kind < right.kind;
            if (left.builder != right.builder)
                return left.builder < right.builder;
            return left.sourceSequence < right.sourceSequence;
        });

    for (size_t begin = 0; begin < intents.size();) {
        const ObjectId bridge = intents[begin].bridge;
        size_t end = begin + 1;
        while (end < intents.size() && intents[end].bridge == bridge) ++end;
        bool ensureCreated = false;
        bool remove = false;
        uint64_t confirmedTick = 0;
        for (size_t index = begin; index < end; ++index) {
            const ObjectBridgeRepairScaffoldIntent& intent = intents[index];
            if (!bridge ||
                intent.confirmedTick !=
                    m_presentation
                        .m_confirmedTick) {
                continue;
            }
            confirmedTick = intent.confirmedTick;
            ensureCreated = ensureCreated ||
                intent.kind ==
                    ObjectBridgeRepairScaffoldIntentKind::EnsureCreated;
            remove = remove ||
                intent.kind == ObjectBridgeRepairScaffoldIntentKind::Remove;
        }
        // A live repair wins a same-tick cancellation from another builder.
        if (confirmedTick != 0 && ensureCreated &&
            !m_bridges.scaffoldingPresent(bridge)) {
            static_cast<void>(m_bridges.createScaffolding(
                bridge, confirmedTick));
            closeCurrentReaction();
        } else if (confirmedTick != 0 && !ensureCreated && remove &&
                   m_bridges.scaffoldingPresent(bridge)) {
            static_cast<void>(m_bridges.removeScaffolding(
                bridge, confirmedTick));
            closeCurrentReaction();
        }
        begin = end;
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleRebuildTargetRemap(WorkItem item) {
    const ObjectRebuildTargetRemapIntent& intent = item.rebuildTargetRemap;
    if (intent.confirmedTick !=
        m_presentation.m_confirmedTick) {
        return true;
    }
    m_targetRemap.remapAttackTargets(intent.from, intent.to);
    retargetStickyBombTargets(intent.from, intent.to);
    return true;
}

bool GameSessionWeaponEventDrain::handleRebuildHoleExpose(WorkItem item) {
    const ObjectRebuildHoleExposeIntent& intent = item.rebuildHoleExpose;
    if (intent.confirmedTick !=
        m_presentation.m_confirmedTick) {
        return true;
    }
    const PlayerState* owner =
        m_content.m_players.get(intent.owner);
    if (!owner || owner->life != PlayerLifeState::Active) return true;

    ObjectSpawnRequest request{
        .templateName = intent.holeTemplate,
        .owner = intent.owner,
        .primaryTeam = intent.team,
        .transform = intent.transform,
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = intent.confirmedTick,
        .geometryOverride = ObjectSpawnGeometryOverride{
            .shape = intent.geometryShape,
            .isSmall = intent.geometryIsSmall,
            .majorRadius = intent.geometryMajorRadius,
            .minorRadius = intent.geometryMinorRadius,
            .height = intent.geometryHeight,
            .boundingCircleRadius = intent.geometryBoundingCircleRadius,
            .boundingSphereRadius = intent.geometryBoundingSphereRadius,
        },
        .producer = intent.source,
        .inheritScriptNamesFrom = intent.source,
    };
    if (intent.holeMaximumHealth > math::q32_32{}) {
        request.maximumHealthOverride = intent.holeMaximumHealth;
    }
    const GameSessionObjectSpawnResult hole =
        m_lifecycle.spawnObject(std::move(request));
    closeCurrentReaction();
    if (!hole ||
        !m_world.m_objectSimulation
             .startRebuildHole(
                 m_world.m_registry,
                 m_world.m_objects,
                 hole.object, intent.rebuildTemplate, intent.source,
                 intent.confirmedTick)) {
        if (hole) {
            static_cast<void>(m_lifecycle.requestDestroyObject(
                hole.object, ObjectDestroyReason::System,
                intent.confirmedTick));
            closeCurrentReaction();
        }
        return true;
    }
    if (intent.transferAttackers) {
        m_targetRemap.remapAttackTargets(intent.source, hole.object);
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleRebuildWorkerSpawn(WorkItem item) {
    const ObjectRebuildWorkerSpawnIntent& intent = item.rebuildWorkerSpawn;
    if (intent.confirmedTick !=
        m_presentation.m_confirmedTick) {
        return true;
    }

    ObjectSpawnRequest workerRequest{
        .templateName = intent.workerTemplate,
        .owner = intent.owner,
        .primaryTeam = intent.team,
        .transform = intent.transform,
        .origin = ObjectCreationOrigin::System,
        .confirmedTick = intent.confirmedTick,
        .initialStatusMask = game::objectStatusBit(
            game::ObjectStatusFlag::Unselectable),
        .producer = intent.hole,
    };
    const GameSessionObjectSpawnResult worker =
        m_lifecycle.spawnObject(std::move(workerRequest));
    closeCurrentReaction();
    if (!worker) {
        static_cast<void>(m_world
            .m_objectSimulation.rejectRebuildWorker(
                m_world.m_registry,
                m_world.m_objects,
                intent.hole, intent.confirmedTick));
        return true;
    }

    ObjectId reconstruction = intent.reconstruction;
    bool createdReconstruction = false;
    if (!reconstruction ||
        !m_world.m_objects.entityFromId(
            reconstruction)) {
        ObjectSpawnRequest rebuildRequest{
            .templateName = intent.rebuildTemplate,
            .owner = intent.owner,
            .primaryTeam = intent.team,
            .transform = intent.transform,
            .origin = ObjectCreationOrigin::System,
            .confirmedTick = intent.confirmedTick,
            .initialStatusMask = game::objectStatusBit(
                game::ObjectStatusFlag::Reconstructing),
            .producer = intent.hole,
            .constructedBy = worker.object,
            .startsUnderConstruction = true,
            .flattenTerrainForStructure = true,
        };
        const GameSessionObjectSpawnResult rebuilt =
            m_lifecycle.spawnObject(std::move(rebuildRequest));
        closeCurrentReaction();
        if (rebuilt) {
            reconstruction = rebuilt.object;
            createdReconstruction = true;
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry,
                    *rebuilt.entity);
            const PlayerState* player =
                m_content.m_players.get(
                    intent.owner);
            const uint32_t requiredFrames = type && type->archetype && player
                ? calculateObjectBuildFrames(
                      *type->archetype, *player,
                      static_cast<uint32_t>(std::max(
                          1, m_content
                                 .m_startInfo.gameSpeedFPS)),
                      m_content
                          .m_objectSimulationRules.energy,
                      intent.confirmedTick)
                : 1u;
            if (!m_world.m_objectSimulation
                     .beginObjectConstruction(
                         m_world.m_registry,
                         m_world.m_objects,
                         reconstruction, worker.object, requiredFrames, true,
                         intent.confirmedTick)) {
                reconstruction = INVALID_OBJECT_ID;
            }
        }
    } else {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(
                reconstruction);
        ObjectConstructionSiteComponent* site = entity
            ? ecs::try_get<ObjectConstructionSiteComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
        if (site) {
            site->builder = worker.object;
            ++site->revision;
            markObjectDirty(
                m_world.m_registry, *entity,
                objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                    objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
        } else {
            reconstruction = INVALID_OBJECT_ID;
        }
    }

    const bool assigned = reconstruction &&
        m_world.m_objectSimulation
            .assignObjectConstruction(
                m_world.m_registry,
                m_world.m_objects,
                worker.object, reconstruction, intent.confirmedTick,
                intent.authoredOrder);
    const bool acknowledged = assigned &&
        m_world.m_objectSimulation
            .acknowledgeRebuildWorker(
                m_world.m_registry,
                m_world.m_objects,
                intent.hole, worker.object, reconstruction,
                intent.confirmedTick);
    if (!acknowledged) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            worker.object, ObjectDestroyReason::System,
            intent.confirmedTick));
        if (createdReconstruction && reconstruction) {
            static_cast<void>(m_lifecycle.requestDestroyObject(
                reconstruction, ObjectDestroyReason::System,
                intent.confirmedTick));
        }
        static_cast<void>(m_world
            .m_objectSimulation.rejectRebuildWorker(
                m_world.m_registry,
                m_world.m_objects,
                intent.hole, intent.confirmedTick));
        closeCurrentReaction();
        return true;
    }

    static_cast<void>(m_lifecycle.evacuateConstructionFootprint(
        reconstruction, worker.object, intent.confirmedTick));

    const std::optional<ecs::entity> holeEntity =
        m_world.m_objects.entityFromId(
            intent.hole);
    if (holeEntity) {
        static_cast<void>(ObjectStatusSystem::apply(
            m_world.m_registry, *holeEntity,
            {.setMask = game::objectStatusBit(
                 game::ObjectStatusFlag::Masked),
             .confirmedTick = intent.confirmedTick}));
    }
    m_targetRemap.remapAttackTargets(intent.hole, reconstruction);
    retargetStickyBombTargets(intent.hole, reconstruction);
    return true;
}

bool GameSessionWeaponEventDrain::handleRebuildCompletion(WorkItem item) {
    const ObjectRebuildCompletionIntent& intent = item.rebuildCompletion;
    if (intent.confirmedTick !=
            m_presentation.m_confirmedTick ||
        !m_world.m_objects.entityFromId(
            intent.reconstruction)) {
        return true;
    }
    static_cast<void>(m_presentation
        .m_scriptObjects.transferObjectNames(
            intent.hole, intent.reconstruction));
    if (intent.worker) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            intent.worker, ObjectDestroyReason::System,
            intent.confirmedTick));
    }
    if (intent.hole) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            intent.hole, ObjectDestroyReason::System,
            intent.confirmedTick));
    }
    closeCurrentReaction();
    return true;
}

} // namespace engine::detail
