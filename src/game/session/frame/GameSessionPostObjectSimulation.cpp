#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/data/base/LineBuildPlacementPlanner.h"
#include "presentation/render/WaterSurfaceVisualSettings.h"

#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/runtime/ObjectDeathEvents.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/structure/ObjectMinefield.h"
#include "game/object/simulation/structure/ObjectMissileLauncherBuilding.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/movement/ObjectWaveGuide.h"

#include <algorithm>
#include <cmath>

namespace engine::detail {
namespace {

[[nodiscard]] render::RenderEntityId constructionPreviewIdentity(
    ObjectId builder, uint32_t sourceSequence) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](uint64_t value) noexcept {
        constexpr uint64_t prime = 1099511628211ull;
        for (uint32_t byte = 0; byte < 8u; ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8u));
            hash *= prime;
        }
    };
    mix(builder.value);
    mix(sourceSequence);
    // Low 48 bits are consumed by the renderer's placement identity domain.
    // Reserve bit 47 for confirmed queued previews so cursor ordinals cannot
    // collide with the same low hash in ordinary sessions.
    return 0x0000800000000000ull |
        (hash & 0x00007fffffffffffull);
}

[[nodiscard]] selection::LocalPlacementPreviewSnapshot
constructionPreviewSnapshot(
    uint64_t presentationEpoch, uint64_t confirmedTick,
    ObjectId builder, const ObjectOrderIntent& order,
    const game::ObjectArchetype& product,
    selection::LocalPlacementLegality legality,
    selection::LocalPlacementPreviewFeedback feedback) {
    selection::LocalPlacementPreviewSnapshot result;
    result.presentationEpoch = presentationEpoch;
    result.animationStartTick = std::min<uint64_t>(
        order.issuedTick, confirmedTick);
    result.previewIdentity = constructionPreviewIdentity(
        builder, order.sourceSequence);
    result.sourceObject = builder;
    result.sourceSequence = order.sourceSequence;
    result.objectType = product.name;
    result.fixedPosition = {
        .x = order.targetX,
        .y = order.targetY,
        .z = order.targetZ,
        .valid = order.hasTargetPosition,
    };
    result.fixedYawRadians = order.placementYawRadians;
    result.position = {
        order.targetX.to_float(), order.targetY.to_float(),
        order.targetZ.to_float()};
    result.yawRadians = order.placementYawRadians.to_float();
    result.fixedLineEndPosition = {
        .x = order.placementEndX,
        .y = order.placementEndY,
        .z = order.placementEndZ,
        .valid = order.hasPlacementEndPosition,
    };
    result.lineEndPosition = {
        order.placementEndX.to_float(),
        order.placementEndY.to_float(),
        order.placementEndZ.to_float()};
    result.hasPose = order.hasTargetPosition;
    result.hasLineEndPosition = order.hasPlacementEndPosition;
    result.legality = legality;
    result.backend = selection::LocalPlacementBackendKind::Build;
    result.feedback = feedback;
    return result;
}

} // namespace

void GameSessionDomainComposition::updatePostCommandObjectSimulation(
    GameSessionPostCombatFrameState& frame) {
    std::erase_if(
        m_presentation.m_rejectedConstructionPlacements,
        [&](const selection::TimedLocalPlacementPreview& preview) {
            return preview.expiresAfterTick <=
                m_presentation.m_confirmedTick;
        });
    auto& clientTerrainMovementSources =
        frame.clientTerrainMovementSources;
    clientTerrainMovementSources.clear();
    const ObjectUpgradeExecutionContext& objectContext = frame.objectContext;
    {
        const auto movingView =
            ecs::view<const ObjectIdentityComponent,
                      const ObjectLifecycleComponent,
                      const TransformComponent>(m_world.m_registry);
        clientTerrainMovementSources.reserve(movingView.size_hint());
        for (const ecs::entity entity : movingView) {
            const ObjectIdentityComponent& identity =
                movingView.template get<const ObjectIdentityComponent>(entity);
            const ObjectLifecycleComponent& lifecycle =
                movingView.template get<const ObjectLifecycleComponent>(entity);
            const TransformComponent& transform =
                movingView.template get<const TransformComponent>(entity);
            if (!identity.id ||
                lifecycle.phase != ObjectLifecyclePhase::Alive ||
                m_world.m_objects.isPendingDestroy(identity.id) ||
                [&] {
                    const ObjectKindOfComponent* kinds =
                        ecs::try_get<ObjectKindOfComponent>(
                            m_world.m_registry, entity);
                    return kinds && game::objectHasKind(
                        kinds->mask, game::ObjectKindOf::Immobile);
                }()) {
                continue;
            }
            clientTerrainMovementSources.push_back({
                .id = identity.id,
                .entity = entity,
                .previousPosition = {
                    transform.x, transform.y, transform.z},
            });
        }
    }
    // Admit Build intents at the session's single structural boundary. The
    // order remains value-only until every content/owner/cash precondition is
    // known, then receives the stable construction-site ObjectId atomically.
    container::Vector<ObjectId> buildersWithPlacement;
    {
        const auto builderView = ecs::view<
            const ObjectIdentityComponent, const ObjectBuilderComponent,
            const ObjectOrderQueueComponent>(m_world.m_registry);
        for (const ecs::entity entity : builderView) {
            const ObjectIdentityComponent& identity =
                builderView.template get<const ObjectIdentityComponent>(entity);
            const ObjectOrderQueueComponent& queue =
                builderView.template get<const ObjectOrderQueueComponent>(entity);
            if (identity.id && !queue.orders.empty() &&
                queue.orders.front().kind == ObjectOrderKind::Build &&
                !queue.orders.front().targetObject &&
                !m_world.m_objectSimulation.isAnyObjectBuilderTaskPending(
                    m_world.m_registry, m_world.m_objects, identity.id)) {
                buildersWithPlacement.push_back(identity.id);
            }
        }
    }
    std::sort(buildersWithPlacement.begin(), buildersWithPlacement.end());
    for (const ObjectId builder : buildersWithPlacement) {
        const std::optional<ecs::entity> builderEntity =
            m_world.m_objects.entityFromId(builder);
        if (!builderEntity) continue;
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *builderEntity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(m_world.m_registry, *builderEntity);
        if (!queue || queue->orders.empty() || !owner ||
            queue->orders.front().kind != ObjectOrderKind::Build ||
            queue->orders.front().targetObject) continue;
        const ObjectOrderIntent order = queue->orders.front();
        const container::SharedPtr<const game::ObjectArchetype> product =
            m_content.m_contentSnapshot.findObjectArchetype(order.contentName);
        const PlayerState* player = m_content.m_players.get(owner->player);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *builderEntity);
        const PlayerId builderOwner = owner->player;
        const ObjectTeamId builderTeam = team ? team->team : INVALID_OBJECT_TEAM_ID;
        const auto publishRejectedPlacement = [&]() {
            if (!product || !order.hasTargetPosition) return;
            const selection::LocalPlacementPreviewSnapshot rejected =
                constructionPreviewSnapshot(
                    m_presentation.m_scriptPresentationEpoch,
                    m_presentation.m_confirmedTick, builder, order,
                    *product, selection::LocalPlacementLegality::Illegal,
                    selection::LocalPlacementPreviewFeedback::Rejected);
            std::erase_if(
                m_presentation.m_rejectedConstructionPlacements,
                [&](const selection::TimedLocalPlacementPreview& existing) {
                    return existing.placement.previewIdentity ==
                        rejected.previewIdentity;
                });
            m_presentation.m_rejectedConstructionPlacements.push_back({
                .placement = rejected,
                .expiresAfterTick =
                    m_presentation.m_confirmedTick + 15u,
            });
            // PlaceEventTranslator gives a human builder one confirmed
            // VoiceNoBuild acknowledgement when the eventual construction
            // admission fails.  It is tied to the rejected order, not the
            // hover preview, so a changing cursor cannot spam speech and a
            // rejected network/script order cannot create a local-only cue.
            const PlayerState* localPlayer =
                m_content.m_players.localPlayer();
            const ThingTemplateComponent* builderType =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *builderEntity);
            if (!localPlayer || localPlayer->id != builderOwner ||
                !builderType || !builderType->archetype) {
                return;
            }
            const container::StringView cue =
                builderType->archetype->templateData.perUnitSound(
                    "VoiceNoBuild");
            if (!cue.empty()) {
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = container::String{cue},
                    .emitter = builder,
                    .owner = builder,
                }));
            }
        };
        const auto rejectPlacement = [&]() {
            const std::optional<ecs::entity> currentEntity =
                m_world.m_objects.entityFromId(builder);
            ObjectOrderQueueComponent* currentQueue = currentEntity
                ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry,
                                                          *currentEntity)
                : nullptr;
            if (!currentQueue || currentQueue->orders.empty() ||
                currentQueue->orders.front().kind != ObjectOrderKind::Build ||
                currentQueue->orders.front().sourceSequence !=
                    order.sourceSequence) return;
            currentQueue->orders.erase(currentQueue->orders.begin());
            ++currentQueue->revision;
        };
        // RefCode refuses an unaffordable construction at the control bar and
        // again in PlaceEventTranslator, announcing EVA_InsufficientFunds each
        // time. Our bar disables the button while the cost is out of reach, so
        // the reachable case is cash drained between the local affordability
        // projection and this confirmed frame. Announce only for the observing
        // player; the authoritative refusal above is unconditional.
        const auto announceInsufficientFunds = [&]() {
            const PlayerState* localPlayer = m_content.m_players.localPlayer();
            if (!localPlayer || localPlayer->id != builderOwner) return;
            GameSessionEvaEventPublisher{m_content, m_publication}.publish(
                audio::EvaEventType::InsufficientFunds,
                m_presentation.m_confirmedTick,
                (static_cast<uint64_t>(builder.value) << 32u) ^
                    static_cast<uint64_t>(order.sourceSequence));
        };
        if (!product || !player || !team || !order.hasTargetPosition) {
            publishRejectedPlacement();
            rejectPlacement();
            continue;
        }
        const bool lineBuild = game::objectHasKind(
            product->kindOfMask, game::ObjectKindOf::LineBuild);
        // The second anchor is the closed protocol discriminator. Never let a
        // stale line payload hitch a ride on an ordinary Build, and never
        // silently degrade a LINEBUILD order into one construction site.
        if (lineBuild != order.hasPlacementEndPosition) {
            publishRejectedPlacement();
            rejectPlacement();
            continue;
        }
        bool ignoreBuildPrerequisites = false;
        const bool buildabilityAdmits = m_production.admitsBuildability(
            builderOwner, *product, ignoreBuildPrerequisites);
        if (!player->constructionPolicy.baseConstructionEnabled ||
            !buildabilityAdmits ||
            !canObjectBuildTemplate(
                m_world.m_registry, *builderEntity,
                m_content.m_contentSnapshot,
                m_presentation.m_scriptCommandBarOverrides,
                m_content.m_players, builderOwner, *product,
                ignoreBuildPrerequisites)) {
            publishRejectedPlacement();
            rejectPlacement();
            continue;
        }
        if (lineBuild) {
            if (!order.hasTargetPosition) {
                rejectPlacement();
                continue;
            }

            struct AuthorityLineContext final {
                GameSessionDomainComposition* session = nullptr;
                const game::ObjectArchetype* product = nullptr;
                ObjectId builder = INVALID_OBJECT_ID;
                PlayerId owner = INVALID_PLAYER_ID;
                math::q32_32 yawRadians{};
            } context{
                .session = this,
                .product = product.get(),
                .builder = builder,
                .owner = builderOwner,
                .yawRadians = order.placementYawRadians,
            };
            LineBuildPlacementRequest lineRequest{
                .start = {order.targetX, order.targetY, order.targetZ},
                .end = {order.placementEndX, order.placementEndY,
                        order.placementEndZ},
                .geometryMajorRadius = product->templateData.geometry
                    .majorRadiusFixed,
                .maxTiles = m_content.m_objectSimulationRules.buildPlacement
                    .maxLineBuildObjects,
                .callbackContext = &context,
                .terrainHeight = [](void* opaque, math::q32_32 x,
                                    math::q32_32 y,
                                    math::q32_32& height) noexcept {
                    AuthorityLineContext& line =
                        *static_cast<AuthorityLineContext*>(opaque);
                    height = math::q32_32::from_raw(
                        line.session->m_content.m_terrain
                            .groundHeightRaw(x.raw(), y.raw()));
                    return true;
                },
                .isLegal = [](void* opaque,
                              const LineBuildPosition& position,
                              uint32_t) noexcept {
                    AuthorityLineContext& line =
                        *static_cast<AuthorityLineContext*>(opaque);
                    const GameSessionBuildPlacementLegalityEvaluation evaluation =
                        line.session->m_placement.evaluateFixed(
                            line.builder,
                            {position.x, position.y, position.z},
                            line.yawRadians, line.owner, *line.product,
                            true, false);
                    return evaluation.evaluated &&
                        evaluation.legality ==
                            selection::LocalPlacementLegality::Legal;
                },
            };
            const LineBuildPlacementPlan linePlan =
                planLineBuildPlacement(lineRequest);
            if (linePlan.legalPrefix.empty()) {
                publishRejectedPlacement();
                rejectPlacement();
                continue;
            }

            // RefCode checks affordability once and buildObjectLineNow creates
            // the already-planned legal prefix immediately. Preserve that
            // dormant LINEBUILD economy rule explicitly; do not multiply the
            // cost by client-visible tile count.
            const int64_t lineCost = calculateObjectBuildCost(
                *product, *player, m_world.m_registry, m_world.m_objects);
            if (lineCost > 0 &&
                !m_content.m_players.trySpend(builderOwner, lineCost)) {
                announceInsufficientFunds();
                publishRejectedPlacement();
                rejectPlacement();
                continue;
            }

            container::Vector<ObjectId> spawnedLine;
            spawnedLine.reserve(linePlan.legalPrefix.size());
            bool lineSpawned = true;
            for (const LineBuildPosition& position :
                 linePlan.legalPrefix) {
                const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject({
                    .templateName = product->name,
                    .owner = builderOwner,
                    .primaryTeam = builderTeam,
                     .transform = ObjectFixedTransformComponent{
                         .position = {position.x, position.y, position.z},
                         .yawRadians = order.placementYawRadians,
                         .authoritative = true,
                     },
                    .origin = ObjectCreationOrigin::Production,
                    .confirmedTick = m_presentation.m_confirmedTick,
                    .producer = builder,
                    .constructedBy = builder,
                    .startsUnderConstruction = true,
                    .flattenTerrainForStructure = true,
                });
                if (!spawned) {
                    lineSpawned = false;
                    break;
                }
                spawnedLine.push_back(spawned.object);
            }
            if (lineSpawned) {
                for (const ObjectId site : spawnedLine) {
                    if (!m_lifecycle.completeConstruction(
                            site, m_presentation.m_confirmedTick)) {
                        lineSpawned = false;
                        break;
                    }
                    static_cast<void>(
                        m_lifecycle.evacuateConstructionFootprint(
                            site, builder, m_presentation.m_confirmedTick));
                }
            }
            if (!lineSpawned) {
                for (const ObjectId site : spawnedLine) {
                    static_cast<void>(m_lifecycle.requestDestroyObject(
                        site, ObjectDestroyReason::System,
                        m_presentation.m_confirmedTick));
                }
                if (lineCost > 0) static_cast<void>(
                    m_content.m_players.adjustCash(builderOwner, lineCost));
                publishRejectedPlacement();
                rejectPlacement();
                continue;
            }
            rejectPlacement();
            continue;
        }
        const math::q32_32 authoritativeYaw =
            order.placementYawRadians;
        const GameSessionBuildPlacementLegalityEvaluation placementEvaluation =
            m_placement.evaluateFixed(
                builder,
                {order.targetX, order.targetY, order.targetZ},
                authoritativeYaw, builderOwner, *product, true, false);
        if (!placementEvaluation.evaluated ||
            placementEvaluation.legality !=
                selection::LocalPlacementLegality::Legal) {
            publishRejectedPlacement();
            rejectPlacement();
            continue;
        }
        const int64_t cost = calculateObjectBuildCost(
            *product, *player, m_world.m_registry, m_world.m_objects);
        if (cost > 0 && !m_content.m_players.trySpend(builderOwner, cost)) {
            announceInsufficientFunds();
            publishRejectedPlacement();
            rejectPlacement();
            continue;
        }
        ObjectSpawnRequest request{
            .templateName = product->name,
            .owner = builderOwner,
            .primaryTeam = builderTeam,
            .transform = ObjectFixedTransformComponent{
                .position = {
                    order.targetX,
                    order.targetY,
                    order.targetZ,
                },
                .yawRadians = authoritativeYaw,
                .authoritative = true,
            },
            .origin = ObjectCreationOrigin::Production,
            .confirmedTick = m_presentation.m_confirmedTick,
            .producer = builder,
            .constructedBy = builder,
            .startsUnderConstruction = true,
            .flattenTerrainForStructure = true,
        };
        const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject(std::move(request));
        const uint32_t requiredFrames = calculateObjectBuildFrames(
            *product, *player,
            static_cast<uint32_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS)),
            m_content.m_objectSimulationRules.energy, m_presentation.m_confirmedTick);
        if (!spawned || !m_world.m_objectSimulation.beginObjectConstruction(
                m_world.m_registry, m_world.m_objects, spawned.object, builder,
                requiredFrames, false, m_presentation.m_confirmedTick) ||
            !m_world.m_objectSimulation.assignObjectConstruction(
                m_world.m_registry, m_world.m_objects, builder, spawned.object,
                m_presentation.m_confirmedTick, order.sourceSequence)) {
            if (spawned) static_cast<void>(m_lifecycle.requestDestroyObject(
                spawned.object, ObjectDestroyReason::System,
                m_presentation.m_confirmedTick));
            if (cost > 0) static_cast<void>(
                m_content.m_players.adjustCash(builderOwner, cost));
            publishRejectedPlacement();
            rejectPlacement();
            continue;
        }
        static_cast<void>(m_lifecycle.evacuateConstructionFootprint(
            spawned.object, builder, m_presentation.m_confirmedTick));
        const PlayerState* localPlayer = m_content.m_players.localPlayer();
        const ThingTemplateComponent* builderType =
            ecs::try_get<ThingTemplateComponent>(
                m_world.m_registry, *builderEntity);
        if (localPlayer && localPlayer->id == builderOwner && builderType &&
            builderType->archetype) {
            const container::StringView cue =
                builderType->archetype->templateData.perUnitSound(
                    "VoiceBuildResponse");
            if (!cue.empty()) {
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = container::String{cue},
                    .emitter = builder,
                    .owner = builder,
                }));
            }
        }
    }

    // Construction occupancy is a lifecycle barrier, not a one-shot spawn
    // side effect. Units may enter a footprint while work is in progress, and
    // a request-local target may temporarily return NoPath. Re-evaluate every
    // live site in stable ObjectId order; an already active evacuation is
    // recognized by its typed purpose and is not duplicated. This provides
    // the retry/final-occupancy behavior without changing generic MoveAside.
    struct ConstructionEvacuationSite final {
        ObjectId site = INVALID_OBJECT_ID;
        ObjectId builder = INVALID_OBJECT_ID;
    };
    container::Vector<ConstructionEvacuationSite> evacuationSites;
    const auto constructionSites = ecs::view<
        const ObjectIdentityComponent,
        const ObjectConstructionSiteComponent>(m_world.m_registry);
    evacuationSites.reserve(constructionSites.size_hint());
    for (const ecs::entity entity : constructionSites) {
        const ObjectIdentityComponent& identity = constructionSites
            .template get<const ObjectIdentityComponent>(entity);
        const ObjectConstructionSiteComponent& site = constructionSites
            .template get<const ObjectConstructionSiteComponent>(entity);
        if (identity.id) {
            evacuationSites.push_back({identity.id, site.builder});
        }
    }
    std::sort(
        evacuationSites.begin(), evacuationSites.end(),
        [](const ConstructionEvacuationSite& left,
           const ConstructionEvacuationSite& right) {
            return left.site < right.site;
        });
    for (const ConstructionEvacuationSite& site : evacuationSites) {
        static_cast<void>(m_lifecycle.evacuateConstructionFootprint(
            site.site, site.builder, m_presentation.m_confirmedTick));
    }

    m_ai.m_objectAIMovementCommands.clear();
    const uint64_t pathRevision = m_content.m_navigation.pathRevision().value;
    for (const ai::MovementCommand& command :
         m_ai.m_objectAI.transients().movementCommands()) {
        m_ai.m_objectAIMovementCommands.push_back({
            .command = command,
            .pathRevision = command.kind == ai::MovementCommandKind::InstallPath
                ? pathRevision
                : 0,
            .mode = command.mode,
            .panicking = command.panicking,
        });
    }
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    const auto closeGameplayPhase = [&] {
        m_damage.resolveQueuedObjectDamage();
        return !m_frame.result().faulted();
    };
    container::Vector<ObjectDamageRequest> neutronDamage;
    const auto updateNeutronSlowDeaths = [&] {
        for (;;) {
            neutronDamage.clear();
            const bool advancedBlast =
                simulation.updateNeutronSlowDeathPhase(
                    m_world.m_registry,
                    m_world.m_objects,
                    m_content.m_terrain,
                    m_presentation.m_confirmedTick, objectContext,
                    neutronDamage);
            // RefCode closes attemptDamage() before visiting the next object
            // in a Blast. Preserve that causal order through the public
            // damage transaction. The slow-death system itself returns after
            // one Blast, so its next pass also observes the complete preceding
            // Blast rather than a precomputed, flattened world view.
            for (ObjectDamageRequest& request : neutronDamage) {
                simulation.queueDamage(std::move(request));
                if (!closeGameplayPhase()) return false;
            }
            if (advancedBlast && neutronDamage.empty() &&
                !closeGameplayPhase()) {
                return false;
            }
            if (!advancedBlast) return true;
        }
    };
    simulation.updateFrameAdmissionPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick, objectContext);
    if (!updateNeutronSlowDeaths()) return;
    simulation.updateMinefieldHazardPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick, objectContext);
    if (!closeGameplayPhase()) return;
    simulation.updateDynamicGeometryHazardPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    simulation.updateFlammableHazardPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    simulation.updatePoisonHazardPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    simulation.finishPoisonHazardPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick);
    simulation.updateOverchargeHazardPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    simulation.updateOrdersAndTacticalPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick, objectContext);
    if (!closeGameplayPhase()) return;
    simulation.updateParticleUplinkPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick, objectContext);
    if (!closeGameplayPhase()) return;
    simulation.updateDynamicSightPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick, objectContext);
    simulation.updateAirOperationsPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick, objectContext);
    for (const ObjectAirfieldAutomaticProductionRequest& request :
         simulation.takeAirfieldAutomaticProductionRequests()) {
        const std::optional<ecs::entity> producer =
            m_world.m_objects.entityFromId(request.producer);
        const OwnerComponent* owner = producer
            ? ecs::try_get<OwnerComponent>(m_world.m_registry, *producer)
            : nullptr;
        if (!owner) continue;
        const GameSessionProductionCommandResult result =
            m_production.queueProduction(
                request.producer, owner->player,
                request.payloadTemplate, request.authoredOrder,
                request.confirmedTick);
        if (result.accepted) {
            static_cast<void>(
                simulation.acknowledgeAirfieldAutomaticProduction(
                    m_world.m_registry, m_world.m_objects, request));
        }
    }
    if (!closeGameplayPhase()) return;
    // WorkerAIUpdate owns one mutually-exclusive Dozer/Supply state machine.
    // Resolve Builder tasks (including bored repair admission) first so the
    // Supply phase observes the confirmed-frame owner instead of performing
    // one stale pickup/delivery action before switching to Dozer.
    simulation.updateConstructionRepairPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick, objectContext);
    if (!closeGameplayPhase()) return;
    simulation.updateSupplyEconomyPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick, objectContext);
    if (!closeGameplayPhase()) return;
    simulation.updateBaseRegenerationPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    simulation.updateAutoHealPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick, objectContext);
    if (!closeGameplayPhase()) return;
    simulation.updateRebuildRecoveryPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    simulation.updateWarehouseRecoveryPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    container::Vector<ObjectLifetimeCommand> lifetimeCommands;
    simulation.prepareLifecyclePhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick, objectContext,
        lifetimeCommands);
    for (const ObjectLifetimeCommand& command : lifetimeCommands) {
        if (!simulation.applyLifetimeCommand(
                m_world.m_registry,
                m_world.m_objects, command,
                m_presentation.m_confirmedTick)) {
            continue;
        }
        if (!closeGameplayPhase()) return;
    }
    simulation.finishLifecyclePhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    // ObjectSimulation remains the sole Health/Armor/Death and
    // Transform/Locomotion writer. AI contributes only sorted value commands.
    // Each producer phase closes through the Session-owned gameplay journal;
    // no subsystem may drain Body/Die reactions behind the journal's back.
    simulation.updateKinematicsPreludePhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick,
        objectContext);
    if (!closeGameplayPhase()) return;
    // This phase may consume movement while its sub-phases publish lifecycle
    // work.  Its ownership test therefore uses a fresh value snapshot rather
    // than a borrowed ObjectAI lane.
    m_ai.m_objectAI.captureOrderCapabilitySnapshot(
        m_ai.m_objectAIOrderCapabilitySnapshot);
    const ObjectKinematicsPhaseState kinematics =
        simulation.updateKinematicsMotionPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick,
        objectContext,
        m_ai.m_objectAIOrderCapabilitySnapshot.moveStopSubjects,
        m_ai.m_objectAIOrderCapabilitySnapshot.attackSubjects,
        m_ai.m_objectAIMovementCommands,
        m_ai.m_objectAI.transients().facingCommands());
    if (!closeGameplayPhase()) return;
    simulation.finishKinematicsPostDamagePhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick,
        kinematics);
    if (!closeGameplayPhase()) return;
    // HeightDie runs in the kinematics chain. A neutron missile killed there
    // receives its SlowDeath runtime only after the early hazard pass, so run
    // the idempotent updater again: already visited runtimes reject this tick,
    // while the newly activated missile publishes FX_Nuke and starts Blast
    // timing in the same frame, matching the legacy UpdateModule chain.
    if (!updateNeutronSlowDeaths()) return;
    simulation.updateBridgeRailPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick);
    if (!closeGameplayPhase()) return;
    simulation.updateSpawnSlavePhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick,
        objectContext);
    if (!closeGameplayPhase()) return;
    simulation.prepareKinematicsCollisionPhase(
        m_world.m_registry,
        m_world.m_objects,
        objectContext);
    simulation.updateSquishCollisionPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick,
        objectContext);
    if (!closeGameplayPhase()) return;
    simulation.finishKinematicsCollisionPhase(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_terrain,
        m_presentation.m_confirmedTick,
        objectContext);
    if (!closeGameplayPhase()) return;
    // Specialized Script Enter/Garrison moves are ordinary authoritative
    // locomotion until they reach the target. Commit the containment edge
    // only after this frame's movement has published its new transform.
    m_scenarioPlans.resolveScriptContainmentEnterIntents();
    m_scenarioPlans.resolveScenarioReinforcementTransportOrders();
    m_scenarioPlans.updateScenarioTeamProductions();
    m_scenarioPlans.updateScenarioTeamAssemblies();

    // Presentation mirrors the confirmed Build queue; it never advances or
    // mutates it. Re-evaluate waiting positions against current terrain,
    // occupancy, prerequisites and cash so a queued yellow footprint turns
    // red as soon as its present-day admission becomes impossible.
    auto& queuedPreviews =
        m_presentation.m_queuedConstructionPlacements;
    queuedPreviews.clear();
    const PlayerState* localPlayer = m_content.m_players.localPlayer();
    if (localPlayer && localPlayer->isCommandPlayer()) {
        const auto view = ecs::view<
            const ObjectIdentityComponent, const OwnerComponent,
            const ObjectBuilderComponent,
            const ObjectOrderQueueComponent>(m_world.m_registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                view.template get<const OwnerComponent>(entity);
            const ObjectOrderQueueComponent& queue =
                view.template get<const ObjectOrderQueueComponent>(entity);
            if (!identity.id || owner.player != localPlayer->id) continue;

            const bool hasQueuedBuild = std::any_of(
                queue.orders.begin(), queue.orders.end(),
                [](const ObjectOrderIntent& order) {
                    return order.kind == ObjectOrderKind::Build &&
                        !order.targetObject && order.hasTargetPosition;
                });
            const ObjectBuilderTask activeBuild =
                m_world.m_objectSimulation.objectBuilderTask(
                    m_world.m_registry, m_world.m_objects, identity.id,
                    ObjectBuilderTaskKind::Build);
            const bool hasActiveBuild =
                activeBuild.kind == ObjectBuilderTaskKind::Build &&
                activeBuild.target;

            // The route begins at the builder's current authoritative
            // position.  It therefore reads Dozer->A->B->C while A is active,
            // and naturally becomes Dozer->B->C as soon as A completes.
            if (hasQueuedBuild || hasActiveBuild) {
                const ObjectFixedTransformComponent* builderTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        m_world.m_registry, entity);
                if (builderTransform && builderTransform->authoritative) {
                    selection::LocalPlacementPreviewSnapshot builderAnchor;
                    builderAnchor.presentationEpoch =
                        m_presentation.m_scriptPresentationEpoch;
                    builderAnchor.animationStartTick =
                        m_presentation.m_confirmedTick;
                    builderAnchor.previewIdentity = identity.id.value;
                    builderAnchor.sourceObject = identity.id;
                    builderAnchor.fixedPosition = {
                        .x = builderTransform->position.x,
                        .y = builderTransform->position.y,
                        .z = builderTransform->position.z,
                        .valid = true,
                    };
                    builderAnchor.position = {
                        builderTransform->position.x.to_float(),
                        builderTransform->position.y.to_float(),
                        builderTransform->position.z.to_float(),
                    };
                    builderAnchor.hasPose = true;
                    builderAnchor.legality =
                        selection::LocalPlacementLegality::Legal;
                    builderAnchor.feedback =
                        selection::LocalPlacementPreviewFeedback::Queued;
                    builderAnchor.routeAnchorOnly = true;
                    queuedPreviews.push_back(std::move(builderAnchor));
                }
            }

            // assignConstruction() consumes the admitted Build order and
            // transfers its identity to ObjectBuilder's active task.  The
            // live site is nevertheless still the first point of the same
            // Shift construction route, so project it before the remaining
            // queued Build orders.  Mark it route-only: the real site already
            // supplies its model and construction bib.
            if (hasActiveBuild) {
                const std::optional<ecs::entity> siteEntity =
                    m_world.m_objects.entityFromId(activeBuild.target);
                const ThingTemplateComponent* siteType = siteEntity
                    ? ecs::try_get<ThingTemplateComponent>(
                          m_world.m_registry, *siteEntity)
                    : nullptr;
                const ObjectFixedTransformComponent* siteTransform = siteEntity
                    ? ecs::try_get<ObjectFixedTransformComponent>(
                          m_world.m_registry, *siteEntity)
                    : nullptr;
                if (siteType && siteType->archetype && siteTransform &&
                    siteTransform->authoritative) {
                    ObjectOrderIntent activeOrder{
                        .kind = ObjectOrderKind::Build,
                        .source = ObjectOrderSource::System,
                        .issuedTick = activeBuild.issuedTick,
                        .sourceSequence = activeBuild.sourceSequence,
                        .targetX = siteTransform->position.x,
                        .targetY = siteTransform->position.y,
                        .targetZ = siteTransform->position.z,
                        .hasTargetPosition = true,
                        .placementYawRadians = siteTransform->yawRadians,
                        .contentName = siteType->archetype->name,
                    };
                    selection::LocalPlacementPreviewSnapshot activePreview =
                        constructionPreviewSnapshot(
                            m_presentation.m_scriptPresentationEpoch,
                            m_presentation.m_confirmedTick, identity.id,
                            activeOrder, *siteType->archetype,
                            selection::LocalPlacementLegality::Legal,
                            selection::LocalPlacementPreviewFeedback::Queued);
                    activePreview.routeAnchorOnly = true;
                    queuedPreviews.push_back(std::move(activePreview));
                }
            }
            for (const ObjectOrderIntent& order : queue.orders) {
                if (order.kind != ObjectOrderKind::Build ||
                    order.targetObject || !order.hasTargetPosition) {
                    continue;
                }
                const container::SharedPtr<const game::ObjectArchetype>
                    product = m_content.m_contentSnapshot.
                        findObjectArchetype(order.contentName);
                if (!product) continue;

                bool legal = true;
                const bool lineBuild = game::objectHasKind(
                    product->kindOfMask, game::ObjectKindOf::LineBuild);
                legal = legal &&
                    lineBuild == order.hasPlacementEndPosition;
                if (legal) {
                    const auto evaluation = m_placement.evaluateFixed(
                        identity.id,
                        {order.targetX, order.targetY, order.targetZ},
                        order.placementYawRadians, localPlayer->id,
                        *product, false);
                    legal = evaluation.evaluated &&
                        evaluation.legality ==
                            selection::LocalPlacementLegality::Legal;
                }
                queuedPreviews.push_back(constructionPreviewSnapshot(
                    m_presentation.m_scriptPresentationEpoch,
                    m_presentation.m_confirmedTick, identity.id, order,
                    *product,
                    legal ? selection::LocalPlacementLegality::Legal
                          : selection::LocalPlacementLegality::Illegal,
                    selection::LocalPlacementPreviewFeedback::Queued));
            }
        }
        std::stable_sort(
            queuedPreviews.begin(), queuedPreviews.end(),
            [](const auto& left, const auto& right) {
                return left.sourceObject < right.sourceObject;
            });
    }
    // WaveGuide modules publish all of this tick's velocity impulses during
    // ObjectSimulation. Advance the client-owned fixed water grid exactly
    // once afterwards, matching W3DWater's legacy confirmed update order.
    static_cast<void>(m_content.m_terrain.advanceVertexWater(
        water_surface::visual_defaults::legacyWaterGravityPerUpdate(
            m_content.m_objectSimulationRules.physics.
                gravityUnitsPerSecondSq.to_float())));
}

} // namespace engine::detail
