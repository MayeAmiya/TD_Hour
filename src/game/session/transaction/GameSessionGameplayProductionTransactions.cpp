#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionObjectProgressionTransactions.h"
#include "game/session/transaction/GameSessionProductionExitRoutes.h"
#include "core/container/string_utils.h"

#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/status/ObjectStealth.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

// Authored `NoSound` is the legacy spelling for "deliberately silent"; RefCode
// rejects it through TheAudio->isValidAudioEvent rather than queueing it.
[[nodiscard]] bool playableMiscAudio(
    container::StringView eventName) noexcept {
    return !eventName.empty() &&
        !container::asciiEqualIgnoreCase(eventName, "NoSound");
}

} // namespace

bool detail::GameSessionWeaponEventDrain::applyProductionSpawnTransaction(
    const ObjectProductionSpawnIntent& intent, bool blockJobSuffix) {
        if (!intent.product || blockJobSuffix)
        {
            m_world.m_objectProduction.releaseSpawnReservation(
                m_world.m_registry, m_world.m_objects, intent.producer,
                intent.exitReservation);
            return blockJobSuffix;
        }
        ObjectSpawnRequest request;
        request.templateName = intent.product->name;
        request.owner = intent.owner;
        request.primaryTeam = intent.targetTeam;
        request.transform = ObjectFixedTransformComponent{
            .position = intent.position,
            .yawRadians = intent.yawRadians,
            .authoritative = true,
        };
        request.initialPathfindLayer = intent.initialPathfindLayer;
        request.origin = ObjectCreationOrigin::Production;
        request.confirmedTick = intent.confirmedTick;
        request.scoreAsBuilt = true;
        request.academyAsProduction = true;
        request.producedBy = ObjectProducedByComponent{
            .producer = intent.producer,
            .productionId = intent.productionId,
            .quantityIndex = intent.quantityIndex,
        };
        const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject(std::move(request));
        // Create callbacks are synchronous in ZH. Close this object's
        // onCreate suffix before exit/parking/ack sees the next product.
        m_lifecycle.resolveQueuedObjectDamage();
        if (!spawned)
        {
            // A transient allocation/ownership failure must not drop or
            // duplicate a completed job.  Leave its unacknowledged suffix at
            // the queue head and retry it deterministically on the next tick.
            m_world.m_objectProduction.releaseSpawnReservation(
                m_world.m_registry, m_world.m_objects, intent.producer,
                intent.exitReservation);
            return true;
        }
        if (ObjectProducerComponent* producer =
                ecs::try_get<ObjectProducerComponent>(
                    m_world.m_registry, *spawned.entity)) {
            producer->producer = intent.producer;
        } else {
            ecs::emplace<ObjectProducerComponent>(
                m_world.m_registry, *spawned.entity,
                ObjectProducerComponent{intent.producer});
        }
        const bool airfieldExit =
            intent.exitReservation.kind ==
                game::ObjectProductionExitKind::AirfieldParking ||
            intent.exitReservation.kind ==
                game::ObjectProductionExitKind::FlightDeck;
        const ObjectKindOfComponent* producedKinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry,
                                                 *spawned.entity);
        if (airfieldExit) {
            const bool producedAtHelipad = producedKinds &&
                game::objectHasKind(
                    producedKinds->mask,
                    game::ObjectKindOf::ProducedAtHelipad);
            if (!producedAtHelipad) {
                ObjectAirfieldReservation parking;
                const bool parkingPlaceExit =
                    intent.exitReservation.kind ==
                        game::ObjectProductionExitKind::AirfieldParking;
                const bool reserved = intent.exitDoorAssigned &&
                        parkingPlaceExit
                    ? m_world.m_objectSimulation.
                          reserveProducedAircraftParkingSlot(
                              m_world.m_registry, m_world.m_objects,
                              intent.producer, spawned.object,
                              intent.exitDoorIndex, intent.confirmedTick,
                              parking)
                    : m_world.m_objectSimulation.reserveAirfieldParkingSlot(
                          m_world.m_registry, m_world.m_objects,
                          intent.producer, spawned.object,
                          intent.confirmedTick, parking);
                if (!reserved) {
                    // Queue admission reserves abstract capacity, while the
                    // concrete ObjectId slot is committed only after central
                    // spawn. A same-frame allied landing may consume that
                    // last slot; retire this unpublished product and retry.
                    static_cast<void>(m_lifecycle.requestDestroyObject(
                        spawned.object, ObjectDestroyReason::System,
                        intent.confirmedTick));
                    m_world.m_objectProduction.releaseSpawnReservation(
                        m_world.m_registry, m_world.m_objects,
                        intent.producer, intent.exitReservation);
                    return true;
                }
            }
            // Both fixed-wing and HeliPark production have a specialized
            // first movement. Fixed-wing taxis from its creation bone;
            // helicopters rise vertically before a queued rally order is
            // restored.
            static_cast<void>(
                m_world.m_objectSimulation.beginProducedAircraftExit(
                    m_world.m_registry, m_world.m_objects,
                    m_content.m_contentSnapshot, spawned.object,
                    intent.confirmedTick));
        }
        production_exit::queueProducedUnitRoute(
            m_world.m_registry, *spawned.entity, intent, intent.confirmedTick,
            m_content.m_navigation);
        if (intent.holdAfterSpawn) {
            static_cast<void>(ObjectDisabledSystem::setUntil(
                m_world.m_registry, *spawned.entity, ObjectDisabledReason::Held,
                std::numeric_limits<uint64_t>::max(), intent.confirmedTick));
        }
        if (intent.inheritProducerKinematics) {
            if (ObjectPhysicsComponent* physics =
                    ecs::try_get<ObjectPhysicsComponent>(m_world.m_registry,
                                                          *spawned.entity)) {
                // QueueProductionExitUpdate describes this kick as making
                // the new object's starting speed equal to the airborne
                // producer's.  The old per-frame integrator achieved that
                // with velocity*mass motive force.  Feeding the same value
                // into a seconds-based force accumulator would multiply the
                // inherited speed by deltaSeconds, so commit the equivalent
                // velocity transfer directly inside this unpublished spawn
                // transaction.
                physics->velocityUnitsPerSecond.x += intent.producerVelocity.x;
                physics->velocityUnitsPerSecond.y += intent.producerVelocity.y;
                physics->velocityUnitsPerSecond.z += intent.producerVelocity.z;
                const uint64_t motiveFrames = std::max<uint64_t>(
                    1u, m_content.m_objectSimulationRules.logicFramesPerSecond / 3u);
                physics->motiveForceExpiresTick = intent.confirmedTick >
                        std::numeric_limits<uint64_t>::max() - motiveFrames
                    ? std::numeric_limits<uint64_t>::max()
                    : intent.confirmedTick + motiveFrames;
                physics->pitchRate = physics->centerOfMassOffset *
                    ObjectPhysicsComponent::Scalar::from_fraction(1, 25);
            }
        }
        if (intent.forceSupplyWanting) {
            if (ObjectEconomyComponent* economy =
                    ecs::try_get<ObjectEconomyComponent>(m_world.m_registry,
                                                          *spawned.entity)) {
                for (ObjectSupplyTruckRuntime& runtime : economy->supplyTrucks) {
                    runtime.scriptIdleSuppressed = false;
                    runtime.externalIdleSuppressed = false;
                    runtime.workerSupplyActive = true;
                    runtime.state =
                        ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                    runtime.targetDock = INVALID_OBJECT_ID;
                    runtime.targetDockModule = 0;
                    runtime.targetIsCenter = false;
                    runtime.approachPosition = -1;
                    runtime.nextActionTick = intent.confirmedTick;
                }
            }
        }
        if (intent.temporaryStealthFrames != 0) {
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(m_world.m_registry,
                                                     *spawned.entity);
            const ObjectStealthComponent* stealth =
                ecs::try_get<ObjectStealthComponent>(m_world.m_registry,
                                                      *spawned.entity);
            const bool canAlreadyStealth = status && status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::CanStealth));
            const bool isTemporaryGrant = stealth &&
                stealth->temporaryGrantExpiresTick > intent.confirmedTick;
            if (stealth && (isTemporaryGrant || !canAlreadyStealth)) {
                static_cast<void>(m_world.m_objectSimulation.grantObjectStealth(
                    m_world.m_registry, m_world.m_objects, spawned.object, true,
                    intent.temporaryStealthFrames, intent.confirmedTick));
            }
        }
        const bool acknowledged = m_world.m_objectProduction.acknowledgeSpawn(
                m_world.m_registry, m_world.m_objects, intent.producer,
                intent.productionId, intent.quantityIndex,
                spawned.object, intent.exitReservation, intent.confirmedTick,
                static_cast<uint32_t>(std::max(
                    1, m_content.m_startInfo.gameSpeedFPS)));
        if (!acknowledged)
        {
            // This should be impossible under the single-threaded confirmed
            // phase.  Keep the created object rather than rolling back an
            // already published lifecycle event, but surface a hard signal.
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Production,
                .code = SimulationFaultCode::AcknowledgementLost,
                .confirmedTick = intent.confirmedTick,
                .subject = intent.producer.value,
                .sequence = intent.productionId,
            }));
            TD_LOG_ERROR("[GameSession] Production spawn acknowledgement lost producer={} job={} quantity={}",
                         intent.producer.value,
                         intent.productionId,
                         intent.quantityIndex);
        }
        else if (intent.targetTeam &&
                 intent.targetTeamRosterIndex != UINT32_MAX &&
                 !m_world.m_objectTeams.recordProductionUnitCompleted(
                     intent.targetTeam,
                     intent.targetTeamRosterIndex))
        {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Production,
                .code = SimulationFaultCode::AcknowledgementLost,
                .confirmedTick = intent.confirmedTick,
                .subject = intent.producer.value,
                .sequence = intent.productionId,
            }));
            TD_LOG_ERROR(
                "[GameSession] Production spawn lost Scenario Team roster correlation team={} roster={} producer={} job={}",
                intent.targetTeam.value,
                intent.targetTeamRosterIndex,
                intent.producer.value,
                intent.productionId);
        }
        if (acknowledged && intent.targetTeam &&
            intent.targetTeamRosterIndex == UINT32_MAX &&
            m_presentation.m_scenarioDefinition) {
            const ObjectTeamRecord* team =
                m_world.m_objectTeams.find(intent.targetTeam);
            const scenario::ScriptTeamDefinition* definition = team &&
                    team->active && team->scenarioDefinition
                ? m_presentation.m_scenarioDefinition->findScriptTeam(
                      team->scenarioDefinition)
                : nullptr;
            if (definition && definition->plan.automaticallyReinforce &&
                !m_world.m_objectTeams.addPendingReinforcement(
                    intent.targetTeam, spawned.object)) {
                static_cast<void>(m_publication.raiseSimulationFault({
                    .domain = SimulationFaultDomain::Production,
                    .code = SimulationFaultCode::AcknowledgementLost,
                    .confirmedTick = intent.confirmedTick,
                    .subject = spawned.object.value,
                    .sequence = intent.productionId,
                }));
                TD_LOG_ERROR(
                    "[GameSession] AutoReinforce product lost pending join correlation team={} object={} producer={} job={}",
                    intent.targetTeam.value, spawned.object.value,
                    intent.producer.value, intent.productionId);
            }
        }
        if (acknowledged) {
            // Strategic WorkOrders retain the exact factory-local production
            // handle. Ordinary and Scenario jobs simply do not match; this
            // prevents same-type production in another authority from being
            // mistaken for the strategic job's completion.
            const std::optional<ecs::entity> producerEntity =
                m_world.m_objects.entityFromId(
                    intent.producer);
            const ObjectProductionComponent* production = producerEntity
                ? ecs::try_get<ObjectProductionComponent>(
                      m_world.m_registry,
                      *producerEntity)
                : nullptr;
            const bool productionStillActive = production &&
                std::any_of(
                    production->jobs.begin(), production->jobs.end(),
                    [&intent](const ObjectProductionJob& job) noexcept {
                        return job.productionId == intent.productionId;
                    });
            static_cast<void>(
                m_ai.m_strategicAI
                    .observeProductionCompletion(
                        intent.producer, intent.productionId,
                        productionStillActive,
                        intent.confirmedTick));
        }
        return false;
}

void detail::GameSessionWeaponEventDrain::applyProductionUpgradeTransaction(
    const ObjectProductionUpgradeCompletionIntent& intent) {
        const UpgradeCatalog* catalog = m_content.m_contentSnapshot.upgradeCatalog();
        const UpgradeDefinition* definition = catalog ? catalog->find(intent.upgrade) : nullptr;
        if (!definition || definition->type != intent.type || !intent.payer)
        {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Production,
                .code = SimulationFaultCode::InvalidEvent,
                .confirmedTick = intent.confirmedTick,
                .subject = intent.producer.value,
                .sequence = intent.sourceSequence,
            }));
            TD_LOG_ERROR("[GameSession] Production completion has invalid upgrade producer={} upgrade={} type={}",
                         intent.producer.value,
                         intent.upgrade.value,
                         static_cast<uint32_t>(intent.type));
            return;
        }

        bool committed = false;
        bool alreadyCompleted = false;
        if (intent.type == UpgradeDefinitionType::Player)
        {
            // A direct script grant may have completed the same technology
            // while this paid job counted down. The queue still drains at its
            // normal completion boundary, but must not fan out twice.
            committed = GameSessionObjectProgressionTransactions{
                m_content, m_world, m_presentation, m_lifecycle.barrier()}
                .commitQueuedPlayerUpgrade(intent.payer, intent.upgrade);
            alreadyCompleted = !committed && m_content.m_players.hasUpgradeComplete(intent.payer, definition->id);
        }
        else
        {
            const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(intent.producer);
            const OwnerComponent* owner = entity ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity) : nullptr;
            const PlayerState* player = owner && owner->player == intent.payer ? m_content.m_players.get(intent.payer) : nullptr;
            const UpgradeMask completedUpgrades = player ? player->upgrades.completed : UpgradeMask{};
            if (entity && player && !m_world.m_objects.isPendingDestroy(intent.producer))
            {
                committed = m_world.m_objectSimulation.completeObjectUpgrade(
                    m_world.m_registry, m_world.m_objects, intent.producer, definition->id, completedUpgrades, intent.confirmedTick,
                    {.players = &m_content.m_players,
                     .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
                     .content = &m_content.m_contentSnapshot,
                     .random = &m_content.m_simulationRandom,
                     .effects = &m_world.m_objectSimulation});
                alreadyCompleted =
                    !committed && m_world.m_objectSimulation.hasObjectUpgrade(m_world.m_registry, *entity, definition->id);
            }
        }
        // UpgradeMux may emit OCL/Weapon/Damage/Spawn work. Close it
        // before this queue head is acknowledged or another factory advances.
        m_lifecycle.resolveQueuedObjectDamage();
        if (!committed && !alreadyCompleted)
        {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Production,
                .code = SimulationFaultCode::AtomicCommitFailed,
                .confirmedTick = intent.confirmedTick,
                .subject = intent.producer.value,
                .sequence = intent.sourceSequence,
            }));
            TD_LOG_ERROR("[GameSession] {} upgrade completion transaction failed producer={} player={} upgrade={}",
                         intent.type == UpgradeDefinitionType::Player ? "PLAYER" : "OBJECT",
                         intent.producer.value,
                         intent.payer.value,
                         intent.upgrade.value);
            return;
        }
        if (intent.paidCost > 0) {
            static_cast<void>(m_content.m_players.recordMoneySpent(
                intent.payer,
                static_cast<uint64_t>(intent.paidCost)));
        }
        static_cast<void>(m_content.m_players.recordAcademyUpgrade(
            intent.payer,
            container::asciiEqualIgnoreCase(definition->academyClassification,
                                  "ACT_UPGRADE_RADAR"),
            false));
        if (!definition->displayNameLabel.empty()) {
            const std::optional<ecs::entity> producerEntity =
                m_world.m_objects.entityFromId(intent.producer);
            const TransformComponent* producerTransform = producerEntity
                ? ecs::try_get<TransformComponent>(
                      m_world.m_registry, *producerEntity)
                : nullptr;
            if (producerTransform) {
                if (m_objectEvents.m_upgradeRadarEpoch !=
                    m_presentation.m_scriptPresentationEpoch) {
                    m_objectEvents.m_upgradeRadarHistory.clear();
                    m_objectEvents.m_upgradeRadarEpoch =
                        m_presentation.m_scriptPresentationEpoch;
                }
                GameSessionObjectEventState::UpgradeRadarPresentationEvent radarEvent;
                radarEvent.producer = intent.producer;
                radarEvent.player = intent.payer;
                radarEvent.position = {
                    producerTransform->x,
                    producerTransform->y,
                    producerTransform->z};
                radarEvent.confirmedTick = intent.confirmedTick;
                radarEvent.sourceSequence = intent.sourceSequence;
                m_objectEvents.m_upgradeRadarHistory.push_back(
                    std::move(radarEvent));
                constexpr size_t maximumUpgradeRadarEvents = 64u;
                if (m_objectEvents.m_upgradeRadarHistory.size() >
                    maximumUpgradeRadarEvents) {
                    m_objectEvents.m_upgradeRadarHistory.erase(
                        m_objectEvents.m_upgradeRadarHistory.begin(),
                        m_objectEvents.m_upgradeRadarHistory.end() -
                            maximumUpgradeRadarEvents);
                }
            }

            // RefCode ProductionUpdate.cpp:917-935 completes this same
            // locally-viewed branch with audio: the authored ResearchSound if
            // the upgrade has one, otherwise the generic EVA_UpgradeComplete
            // advisor line, and then UnitSpecificSound unconditionally. Both
            // keys parse here but had zero readers, and
            // EvaEvent::UpgradeComplete was never emitted, so finishing an
            // upgrade was completely silent.
            //
            // isLocallyViewed() is the observed-or-local controlling player;
            // for a paid upgrade job the producer's owner is the payer (that
            // identity is asserted above for the object-scoped branch), so the
            // local-player comparison is the faithful equivalent. Audio is
            // presentation-only and never touches SimulationRandom.
            const PlayerState* localPlayer =
                m_content.m_players.localPlayer();
            if (localPlayer && localPlayer->id == intent.payer) {
                if (playableMiscAudio(definition->researchCompleteSound)) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = definition->researchCompleteSound,
                        .emitter = intent.producer,
                        .owner = intent.producer,
                    }));
                } else {
                    GameSessionEvaEventPublisher{m_content, m_publication}
                        .publish(
                            audio::EvaEventType::UpgradeComplete,
                            intent.confirmedTick,
                            (static_cast<uint64_t>(intent.producer.value)
                                 << 32u) ^
                                static_cast<uint64_t>(definition->id.value));
                }
                if (playableMiscAudio(definition->unitSpecificSound)) {
                    static_cast<void>(m_publication.emitAudioEvent({
                        .eventName = definition->unitSpecificSound,
                        .emitter = intent.producer,
                        .owner = intent.producer,
                    }));
                }
                if (intent.type == UpgradeDefinitionType::Object) {
                    // WeaponUpgradeSound is retained by Generals/ZH only as a
                    // DefaultThingTemplate NoSound entry; retail has no
                    // reachable reader.  Give the authored extension one
                    // stable meaning: the confirmed completion of this exact
                    // object's ObjectUpgrade production job.  It is
                    // deliberately not a PlayerUpgrade fan-out (which could
                    // speak once per owned unit) and not a weapon-set poll.
                    const std::optional<ecs::entity> upgraded =
                        m_world.m_objects.entityFromId(intent.producer);
                    const ThingTemplateComponent* type = upgraded
                        ? ecs::try_get<ThingTemplateComponent>(
                            m_world.m_registry, *upgraded)
                        : nullptr;
                    const container::StringView cue = type && type->archetype
                        ? type->archetype->templateData.perUnitSound(
                              "WeaponUpgradeSound")
                        : container::StringView{};
                    if (playableMiscAudio(cue)) {
                        static_cast<void>(m_publication.emitAudioEvent({
                            .eventName = container::String{cue},
                            .emitter = intent.producer,
                            .owner = intent.producer,
                        }));
                    }
                }
            }
        }
        // ProductionUpdate notifies ScriptEngine before it removes the queue
        // entry. Preserve that consumable (player, internal name, producer)
        // fact even if the later acknowledgement reports an invariant fault.
        static_cast<void>(m_presentation.m_scriptGameplayEvents.recordUpgrade({
            .player = intent.payer,
            .source = intent.producer,
            .upgrade = definition->name,
            .confirmedTick = intent.confirmedTick,
        }));
        if (!m_world.m_objectProduction.acknowledgePlayerUpgrade(m_world.m_registry, m_world.m_objects, intent.producer, intent.upgrade))
        {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::Production,
                .code = SimulationFaultCode::AcknowledgementLost,
                .confirmedTick = intent.confirmedTick,
                .subject = intent.producer.value,
                .sequence = intent.sourceSequence,
            }));
            TD_LOG_ERROR("[GameSession] {} upgrade acknowledgement lost producer={} player={} upgrade={}",
                         intent.type == UpgradeDefinitionType::Player ? "PLAYER" : "OBJECT",
                         intent.producer.value,
                         intent.payer.value,
                         intent.upgrade.value);
        }
}

} // namespace engine

namespace engine::detail {

bool GameSessionWeaponEventDrain::handleProductionSpawn(WorkItem item) {
    const ObjectProductionSpawnIntent& intent = item.productionSpawn;
    const bool blockedSuffix =
        intent.producer == m_blockedProductionProducer &&
        intent.productionId == m_blockedProductionId;
    const bool blockJob = applyProductionSpawnTransaction(
        intent, blockedSuffix);
    closeCurrentReaction();
    if (blockJob) {
        m_blockedProductionProducer = intent.producer;
        m_blockedProductionId = intent.productionId;
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleProductionUpgrade(WorkItem item) {
    applyProductionUpgradeTransaction(item.productionUpgrade);
    closeCurrentReaction();
    return true;
}

} // namespace engine::detail
