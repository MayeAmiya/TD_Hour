#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/frame/GameSessionProjectilePresentationRules.h"
#include "game/session/frame/GameSessionWeaponEventPublisher.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/transaction/GameSessionAIAttackOrderTransactions.h"
#include "game/session/query/GameSessionAIOrderPolicy.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"

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
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/simulation/structure/ObjectParticleUplinkCannon.h"
#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/movement/ObjectWaveGuide.h"

#include <chrono>

namespace engine::detail {

void GameSessionDomainComposition::resolvePostCommandCombat(
    GameSessionPostCombatFrameState& frame) {
#if TD_DEBUG_ENABLED
    const auto resolveStarted = std::chrono::steady_clock::now();
#endif
    frame.repairVisibilitySnapshot = m_world.m_mapVisibility.snapshot();
    frame.objectContext = {
        .players = &m_content.m_players,
        .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
        .content = &m_content.m_contentSnapshot,
        .aiTargetPriority = {
            .context = this,
            .resolve = [](const void* context, ObjectId subject,
                          ecs::entity target) noexcept {
                const auto& composition =
                    *static_cast<const GameSessionDomainComposition*>(context);
                return GameSessionAIOrderPolicy{
                    composition.m_content, composition.m_world,
                    composition.m_presentation}
                    .attackPriorityForTarget(subject, target);
            },
        },
        .rankLevelLimit = m_presentation.m_scriptRankLevelLimit,
        .random = &m_content.m_simulationRandom,
        .terrain = &m_content.m_terrain,
        .spatialIndex = &m_world.m_spatialIndex,
        .navigation = &m_content.m_navigation,
        .mapVisibility = frame.repairVisibilitySnapshot.get(),
        .mapVisibilityAuthority =
            &m_world.m_mapVisibility,
        .effects = &m_world.m_objectSimulation,
    };
    const ObjectUpgradeExecutionContext& objectContext = frame.objectContext;
    // RefCode's PartitionManager is maintained as objects move. Our snapshot
    // index is rebuilt at explicit deterministic boundaries, so refresh it
    // once after scripts/commands before AI and proximity systems consume it.
    m_world.m_spatialIndex.refreshDirty(
        m_world.m_registry,
        m_world.m_objects);
    // EMP is authoritative status production. Commit its disable/air-kill
    // transactions before Combat and aura sources sample Disabled this tick.
    m_world.m_objectSimulation.updatePreCombatStatusEffects(
        m_world.m_registry, m_world.m_objects, m_content.m_players, m_content.m_simulationRandom,
        m_presentation.m_confirmedTick, objectContext);
    m_damage.resolveQueuedObjectDamage();
    // Aura-style temporary bonuses must be present before CombatSystem
    // resolves this frame's weapon scalars. The same boundary is idempotent
    // when ObjectSimulation::update is reached later in this method.
    m_world.m_objectSimulation.updateWeaponBonuses(
        m_world.m_registry, m_world.m_objects, m_content.m_players, m_content.m_contentSnapshot,
        m_content.m_simulationRandom, m_presentation.m_confirmedTick);
#if TD_DEBUG_ENABLED
    const auto statusFinished = std::chrono::steady_clock::now();
#endif
    // Navigation feedback is polled before AI and is therefore visible once
    // in this phase. Requests emitted below are submitted afterwards and can
    // never feed back into the same confirmed tick.
    // Observe Stop/replacement before polling. This converts an already
    // submitted old request into a Cancel tombstone (or prepares replacement)
    // before a Ready PathHandle can be consumed into an obsolete AI inbox.
    m_aiMoveOrders.observeOrders();
    GameSessionAIAttackOrderTransactions{
        m_content,
        m_world,
        m_ai,
        m_presentation}
        .observeAttackOrders();
    m_aiAttackOrders.observeTacticalAttackOrders();
    m_aiInsertion.observeOrders();
    m_aiNavigation.pollFeedback();
    if (m_frame.result().faulted()) return;
    m_aiInsertion.stageMotionFeedback();
    if (m_frame.result().faulted()) return;
    m_aiShadow.run();
    if (m_frame.result().faulted()) return;
    m_aiSpecialCommands.resolve();
    if (m_frame.result().faulted()) return;
    m_aiResolution.resolveGuardCommands();
    if (m_frame.result().faulted()) return;
    m_aiResolution.resolveOpportunityQueries();
    if (m_frame.result().faulted()) return;
    m_aiResolution.resolveTacticalAttackQueries();
    if (m_frame.result().faulted()) return;
    m_aiNavigation.submitRequests();
    if (m_frame.result().faulted()) return;
#if TD_DEBUG_ENABLED
    const auto aiFinished = std::chrono::steady_clock::now();
#endif
    // Combat closes gameplay transactions after each actor.  Those
    // transactions may retire or add ObjectAI members, so capture both
    // ownership and already-emitted Attack commands by value before handing
    // them to the long-running Combat loop.
    m_ai.m_objectAI.captureOrderCapabilitySnapshot(
        m_ai.m_objectAIOrderCapabilitySnapshot);
    m_ai.m_objectAIAttackCommandSnapshot.assign(
        m_ai.m_objectAI.transients().attackCommands().begin(),
        m_ai.m_objectAI.transients().attackCommands().end());
    // Preserve the original object-update boundary after scripts, terrain and
    // confirmed player commands. Combat consumes newly admitted Attack intents
    // first and emits value-only damage into ObjectSimulation, so weapon hits
    // participate in this exact frame's armor/Body/death ordering rather than
    // receiving a one-tick delay behind their command.
    const bool combatCompleted = m_world.m_objectCombat.update(
        m_world.m_registry,
        m_world.m_objects,
        m_content.m_contentSnapshot,
        &m_world.m_spatialIndex,
        &m_content.m_players,
        m_content.m_simulationRandom,
        static_cast<uint32_t>(std::max(
            1, m_content.m_startInfo.gameSpeedFPS)),
        m_presentation.m_confirmedTick,
        {.context = this,
         .submit = [](void* context,
                      ObjectSystemWeaponFireCommand&& command) {
             auto& owner = *static_cast<GameSessionDomainComposition*>(context);
             // Combat's local sequence orders only this actor's siblings.
             // Admission into the shared gameplay stack receives the one
             // owner-thread ordinal, then closes before Combat inspects the
             // next stable ObjectId.
             command.emissionSequence = owner.m_world
                 .m_objectSimulation.reserveGameplaySubmissionOrdinal();
             owner.m_world.m_objectSimulation
                 .queueSystemWeaponFireCommand(std::move(command));
             owner.drainGameplayTransactions();
             if (owner.m_frame.result().faulted()) return false;
             // Spawn/Destroy/geometry suffixes from this actor are visible to
             // the next actor through the same maintained broad phase.
             owner.m_world.m_spatialIndex.refreshDirty(
                 owner.m_world.m_registry,
                 owner.m_world.m_objects);
             return true;
         }},
        ObjectCombatAIInput{
            .owners = m_ai.m_objectAIOrderCapabilitySnapshot.attackSubjects,
            .autonomousOwners = m_ai.m_objectAIOrderCapabilitySnapshot
                .autonomousAttackSubjects,
            .commands = m_ai.m_objectAIAttackCommandSnapshot,
            // KINDOF_ATTACK_NEEDS_LINE_OF_SIGHT consults the published dynamic
            // obstacle field; AIData owns the global enable.
            .navigation = &m_content.m_navigation,
            .attackUsesLineOfSight = m_content.m_objectSimulationRules.ai
                .attackUsesLineOfSight,
            .aiCrushesInfantry = m_content.m_objectSimulationRules.ai
                .aiCrushesInfantry,
        });
    if (!combatCompleted || m_frame.result().faulted()) return;
    GameSessionWeaponEventPublisher{
        m_content,
        m_world,
        m_presentation,
        m_publication}
        .publish(
            m_world.m_objectCombat.takeEvents());
#if TD_DEBUG_ENABLED
    const auto combatFinished = std::chrono::steady_clock::now();
#endif
    m_ai.m_objectAI.transients().discardAttackCommands();
    for (const ai::AIAttackFeedback& feedback :
         m_world.m_objectCombat.takeAIAttackFeedback()) {
        if (feedback.kind == ai::AIAttackFeedbackKind::FireCompleted) {
            const std::optional<ai::ObjectAIActorStateView> actor =
                m_ai.m_objectAI.actorState(feedback.correlation.subject);
            if (actor && actor->state == ai::AIStateId::GuardTunnelNetwork &&
                feedback.correlation.state ==
                    ai::AIStateId::AttackAndFollowObject &&
                feedback.target) {
                // AITNGuardAttackAggressorState refreshes TunnelTracker only
                // while its nested attack machine is actually firing.
                static_cast<void>(m_world.m_objectSimulation
                    .publishTunnelNetworkNemesis(
                        m_world.m_registry, m_world.m_objects,
                        feedback.correlation.subject, feedback.target,
                        m_presentation.m_confirmedTick));
            }
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(feedback.correlation.subject);
            ObjectOrderQueueComponent* queue = entity
                ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
                : nullptr;
            if (queue && !queue->orders.empty()) {
                ObjectOrderIntent& order = queue->orders.front();
                const ai::AIAsyncOrderIdentity& expected =
                    feedback.correlation.orderIdentity;
                const bool matches =
                    order.kind == ObjectOrderKind::Attack &&
                    expected.subject == feedback.correlation.subject &&
                    expected.queueRevision == queue->revision &&
                    expected.externalRevision == queue->externalRevision &&
                    expected.issuedTick == order.issuedTick &&
                    expected.sourceSequence == order.sourceSequence &&
                    expected.sourceScriptId == order.sourceScriptId &&
                    expected.systemPurposeInstance ==
                        order.systemPurposeInstance &&
                    expected.source == static_cast<uint8_t>(order.source) &&
                    expected.systemPurpose ==
                        static_cast<uint8_t>(order.systemPurpose);
                if (matches && order.maximumShots &&
                    order.shotsFired !=
                        std::numeric_limits<uint32_t>::max()) {
                    ++order.shotsFired;
                }
            }
        }
        const ai::ObjectAITransientStatus staged =
            m_ai.m_objectAI.transients().stage(feedback);
        if (staged != ai::ObjectAITransientStatus::Success) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Feedback,
                .code = staged == ai::ObjectAITransientStatus::CapacityExceeded
                    ? SimulationFaultCode::CapacityExceeded
                    : SimulationFaultCode::InvalidEvent,
                .confirmedTick = m_presentation.m_confirmedTick,
                .subject = feedback.correlation.subject.value,
            }));
            TD_LOG_ERROR(
                "[GameSession] Object AI Combat feedback overflow: "
                "subject={} tick={} status={}",
                feedback.correlation.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(staged));
            return;
        }
    }
    // Attack exit cleanup commands have now crossed the Combat authority
    // boundary, so the exact admission/queue-head completion can commit.
    m_aiAttackOrders.commitAttackCompletions();
    // Every Combat actor's FireOCL/projectile/direct-Damage transaction was
    // already closed by the sink above before the following actor ran.
    m_countermeasures.updateAndResolveDiversions();
#if TD_DEBUG_ENABLED
    const auto weaponHandoffFinished = std::chrono::steady_clock::now();
#endif
    m_world.m_objectProjectiles.update(m_world.m_registry, m_world.m_objects, m_content.m_contentSnapshot, &m_world.m_spatialIndex, &m_content.m_players,
                               m_content.m_terrain, static_cast<uint32_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS)),
                               m_presentation.m_confirmedTick);
#if TD_DEBUG_ENABLED
    const auto projectileUpdateFinished = std::chrono::steady_clock::now();
#endif
    container::Vector<ObjectProjectileGameplayTransaction>
        projectileTransactions =
            m_world.m_objectProjectiles
                .takeGameplayTransactions();
    for (ObjectProjectileGameplayTransaction& transaction :
         projectileTransactions) {
        bool queuedOcl = false;
        for (const ObjectProjectileGameplayEvent& event :
             transaction.events) {
            if (event.kind ==
                    ObjectProjectileEventKind::GarrisonCleared &&
                event.owner) {
                static_cast<void>(
                    m_content.m_players
                        .recordClearedGarrisonedBuilding(event.owner));
            }
            if (!session_projectile::invokesDetonationFx(event.kind)) continue;
            const game::WeaponTemplate* definition =
                m_content.m_contentSnapshot.findWeapon(
                    event.detonationWeapon);
            if (!definition || !event.owner || !event.primaryTeam) continue;
            const size_t veterancyIndex = std::min<size_t>(
                static_cast<size_t>(event.veterancy),
                game::WeaponTemplate::kVeterancyLevelCount - 1);
            const game::ObjectCreationListContentId detonationOcl =
                definition->projectileDetonationOclIds[veterancyIndex];
            if (!detonationOcl) continue;
            m_world.m_objectSimulation
                .queueObjectCreationListInvocation({
                    .content = detonationOcl,
                    .source = event.projectile,
                    .owner = event.owner,
                    .primaryTeam = event.primaryTeam,
                    .primaryPosition = event.position,
                    .sourceVelocity = event.sourceVelocity,
                    .orientationRadians = event.orientationRadians,
                    .pitchRadians = event.pitchRadians,
                    .rollRadians = event.rollRadians,
                    .veterancy = static_cast<game::ObjectVeterancyLevel>(
                        veterancyIndex),
                    .authoredOrder = event.sourceShotSequence,
                    .emissionSequence = m_world
                        .m_objectSimulation.reserveGameplaySubmissionOrdinal(),
                    .confirmedTick = event.confirmedTick,
                    .sourcePathfindLayer = event.sourcePathfindLayer,
                    .sourceAirborne = event.sourceAirborne,
                    .sourceOwnsFullAttitude =
                        event.sourceOwnsFullAttitude,
                });
            queuedOcl = true;
        }
        if (queuedOcl) {
            // ProjectileDetonationOCL is the projectile form of FireOCL and
            // precedes warhead Damage in Weapon::fireWeaponTemplate.
            drainGameplayTransactions();
            if (m_frame.result().faulted()) return;
        }
        for (const ObjectHistoricBonusWeaponFire& bonus :
             transaction.historicBonusWeapons) {
            m_world.m_objectSimulation.queueSystemWeaponFireCommand(
                {
                    .source = bonus.source,
                    .content = bonus.content,
                    .sourcePosition = bonus.position,
                    .impactPosition = bonus.position,
                    .sourceShotSequence = bonus.sourceSequence,
                    .authoredOrder = bonus.authoredOrder,
                    .emissionSequence = m_world.m_objectSimulation
                        .reserveGameplaySubmissionOrdinal(),
                    .confirmedTick = bonus.confirmedTick,
                });
            // HistoricBonusWeapon executes synchronously before the current
            // impact's ordinary damage, matching Weapon::dealDamageInternal.
            drainGameplayTransactions();
            if (m_frame.result().faulted()) return;
        }
        for (ObjectDamageRequest& request : transaction.damage) {
            m_world.m_objectSimulation.queueDamage(
                std::move(request));
        }
        m_damage.resolveQueuedObjectDamage();
        if (m_frame.result().faulted()) return;
    }
    container::Vector<ObjectProjectileEvent> projectileEvents = m_world.m_objectProjectiles.takeEvents();
    for (const ObjectProjectileEvent& event : projectileEvents) {
        const math::vec3 eventPresentationPosition{
            event.position.x.to_float(),
            event.position.y.to_float(),
            event.position.z.to_float()};
        if (event.kind == ObjectProjectileEventKind::GroundDecalBegin ||
            event.kind == ObjectProjectileEventKind::GroundDecalEnd) {
            PlayerId owner = INVALID_PLAYER_ID;
            std::optional<ecs::entity> ownerEntity =
                m_world.m_objects.entityFromIdIncludingPending(event.projectile);
            if (!ownerEntity) {
                ownerEntity =
                    m_world.m_objects.entityFromIdIncludingPending(event.launcher);
            }
            if (ownerEntity) {
                if (const OwnerComponent* component =
                        ecs::try_get<OwnerComponent>(m_world.m_registry,
                                                     *ownerEntity)) {
                    owner = component->player;
                }
            }
            if (owner) {
                m_objectEvents.m_projectileRadiusDecalEvents.push_back({
                    .kind = event.kind ==
                            ObjectProjectileEventKind::GroundDecalBegin
                        ? ObjectRadiusDecalEventKind::Begin
                        : ObjectRadiusDecalEventKind::End,
                    .source =
                        ObjectRadiusDecalEventSource::NeutronMissileUpdate,
                    .object = event.projectile,
                    .owner = owner,
                    .authoredOrder = event.authoredOrder,
                    .texture = event.decalTexture,
                    .position = event.position,
                    .radius = event.decalRadius,
                    .shadowTypeMask = event.decalShadowTypeMask,
                    .minimumOpacity = event.decalMinimumOpacity,
                    .maximumOpacity = event.decalMaximumOpacity,
                    .opacityThrobTicks = event.decalOpacityThrobFrames,
                    .color = event.decalColor,
                    .usesPlayerColor = event.decalUsesPlayerColor,
                    .onlyVisibleToOwningPlayer =
                        event.decalOnlyVisibleToOwningPlayer,
                    .confirmedTick = event.confirmedTick,
                });
            }
            continue;
        }
        if (event.kind == ObjectProjectileEventKind::Effect) {
            // Launch/ignition/trail events freeze their policy at the
            // confirmed simulation point. Never re-derive stealth or delay
            // from end-of-frame state, and never let this generic handoff
            // bypass a suppressed weapon policy.
            if (event.weaponFxPolicy != game::WeaponFxPolicy::Play) continue;
            if (event.fxListName.empty() && event.particleSystemName.empty()) continue;
            // Ignition is a one-shot world effect at the sealed launch point.
            // ProjectileExhaust is the sole direct-particle producer in this
            // event family and must instead remain attached to the projectile
            // throughout its flight.  Keeping these two authored resources
            // distinct prevents a trailing emitter from being frozen at the
            // first launch position.
            const game::FxInvocationAnchor ignitionAnchor = session_fx::worldAnchor(
                eventPresentationPosition, event.projectile);
            constexpr game::FxInvocationAnchorKind ignitionAnchorKind =
                game::FxInvocationAnchorKind::WorldPosition;
            [[maybe_unused]] bool fxAccepted = true;
            if (!event.fxListName.empty()) {
                fxAccepted = m_publication.emitFxInvocationEvent({
                    .fxListName = event.fxListName,
                    .anchorKind = ignitionAnchorKind,
                    .primary = ignitionAnchor,
                });
            }
            [[maybe_unused]] bool particleAccepted = true;
            if (!event.particleSystemName.empty()) {
                const std::optional<game::FxInvocationAnchor> projectileAnchor =
                    session_fx::snapshotAnchor(
                        m_world.m_registry, m_world.m_objects,
                        event.projectile);
                particleAccepted = m_publication.emitFxInvocationEvent({
                    .directParticle = game::FxDirectParticleRequest{
                        .particleSystemName = event.particleSystemName,
                        .emitterCount = 1,
                        .systemLifetimeFrames =
                            event.particleSystemLifetimeFrames != 0
                                ? std::optional<uint32_t>{
                                       event.particleSystemLifetimeFrames}
                                : std::nullopt,
                        .attachToObject = projectileAnchor.has_value(),
                    },
                    .anchorKind = projectileAnchor
                        ? game::FxInvocationAnchorKind::ObjectAttachment
                        : ignitionAnchorKind,
                    .primary = projectileAnchor.value_or(ignitionAnchor),
                    .localVisibilityRetryFrames =
                        std::numeric_limits<uint32_t>::max(),
                });
            }
#if TD_DEBUG_ENABLED
            TD_LOG_DEBUG(
                "[GameSession] Projectile effect handoff: projectile={} tick={} fx='{}' fxAccepted={} particle='{}' particleAccepted={} anchorKind={} position=({}, {}, {})",
                event.projectile.value, event.confirmedTick,
                event.fxListName, fxAccepted, event.particleSystemName,
                particleAccepted, static_cast<uint32_t>(ignitionAnchorKind),
                ignitionAnchor.position.x(), ignitionAnchor.position.y(),
                ignitionAnchor.position.z());
#endif
            continue;
        }
        if (event.kind == ObjectProjectileEventKind::GarrisonCleared) {
            if (event.fxListName.empty()) continue;
            const std::optional<game::FxInvocationAnchor> targetAnchor =
                session_fx::snapshotAnchor(
                    m_world.m_registry, m_world.m_objects, event.target);
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = event.fxListName,
                .anchorKind = targetAnchor
                    ? game::FxInvocationAnchorKind::ObjectAttachment
                    : game::FxInvocationAnchorKind::WorldPosition,
                .primary = targetAnchor.value_or(
                    session_fx::worldAnchor(
                        eventPresentationPosition, event.target)),
            }));
            continue;
        }
        if (!session_projectile::invokesDetonationFx(event.kind)) continue;
        // Neutron missiles force-kill their projectile on collision and let
        // NeutronMissileSlowDeathBehavior own the full nuclear presentation.
        // Keep the generic damage/OCL transaction above intact, but do not
        // also publish ProjectileDetonationFX here: both FXLists can contain
        // Sound nuggets, producing a duplicate blast and audio command.
        const std::optional<ecs::entity> projectileEntity =
            event.kind == ObjectProjectileEventKind::Collided
            ? m_world.m_objects.entityFromIdIncludingPending(event.projectile)
            : std::nullopt;
        if (projectileEntity &&
            ecs::try_get<ObjectNeutronMissileProjectileComponent>(
                m_world.m_registry, *projectileEntity)) {
            continue;
        }
        const game::WeaponTemplate* definition =
            m_content.m_contentSnapshot.findWeapon(event.detonationWeapon);
        if (!definition) continue;

        game::FxInvocationAnchor primary =
            session_fx::snapshotAnchor(m_world.m_registry, m_world.m_objects, event.projectile)
                .value_or(session_fx::worldAnchor(
                    eventPresentationPosition, event.projectile));
        // The detonation event owns the exact contact/path position. Keep any
        // copied projectile attitude, but do not replace this point with a
        // potentially stale TransformComponent projection.
        primary.position = eventPresentationPosition;
        const size_t veterancyIndex = std::min<size_t>(
            static_cast<size_t>(event.veterancy),
            game::WeaponTemplate::kVeterancyLevelCount - 1);
        const container::String& detonationFxName =
            definition->projectileDetonationFXs[veterancyIndex];
        if (event.weaponFxPolicy == game::WeaponFxPolicy::Play &&
            !detonationFxName.empty()) {
            static_cast<void>(m_publication.emitFxInvocationEvent({
                .fxListName = detonationFxName,
                .anchorKind = game::FxInvocationAnchorKind::WorldPosition,
                .primary = primary,
                .secondary = session_fx::snapshotAnchor(
                    m_world.m_registry, m_world.m_objects, event.target),
                .primarySpeed = definition->fixed.weaponSpeed.to_float(),
                .overrideRadius =
                    definition->fixed.primaryDamageRadius.to_float(),
            }));
        }

    }
#if TD_DEBUG_ENABLED
    const auto resolveFinished = std::chrono::steady_clock::now();
    if (m_presentation.m_confirmedTick <= 16u) {
        const auto micros = [](auto begin, auto end) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count();
        };
        TD_LOG_INFO(
            "[CombatResolveTiming] tick={} status={}us ai={}us combat={}us handoff={}us projectiles={}us events={}us total={}us",
            m_presentation.m_confirmedTick,
            micros(resolveStarted, statusFinished),
            micros(statusFinished, aiFinished),
            micros(aiFinished, combatFinished),
            micros(combatFinished, weaponHandoffFinished),
            micros(weaponHandoffFinished, projectileUpdateFinished),
            micros(projectileUpdateFinished, resolveFinished),
            micros(resolveStarted, resolveFinished));
    }
#endif
}

} // namespace engine::detail
