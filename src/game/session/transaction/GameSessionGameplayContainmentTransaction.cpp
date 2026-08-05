#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionProductionExitRoutes.h"
#include "game/object/simulation/economy/ObjectProduction.h"

#include <algorithm>

namespace engine::detail {

bool GameSessionWeaponEventDrain::handleContainmentEvent(WorkItem item) {
    const ObjectContainmentEvent& event = item.containmentEvent;
    const auto finish = [this]() {
        closeCurrentReaction();
        return true;
    };
    if (!event.accepted) return finish();

    if (event.kind == ObjectContainmentRequestKind::Detach &&
        event.parachuteLandingTransport && event.object) {
        const std::optional<ecs::entity> rider =
            m_world.m_objects.entityFromId(event.object);
        const OwnerComponent* owner = rider
            ? ecs::try_get<OwnerComponent>(m_world.m_registry, *rider)
            : nullptr;
        if (rider && owner && owner->player) {
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(
                    m_world.m_registry, *rider);
            if (!queue) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    m_world.m_registry, *rider);
            }
            queue->orders.clear();
            ++queue->revision;
            ecs::remove<ObjectSystemPathSequenceComponent>(
                m_world.m_registry, *rider);

            const PlayerState* player =
                m_content.m_players.get(owner->player);
            if (player && player->controller == PlayerControllerKind::Ai) {
                queue->orders.push_back({
                    .kind = ObjectOrderKind::TacticalAttack,
                    .tacticalAttackSubtype =
                        ObjectTacticalAttackSubtype::Hunt,
                    .source = ObjectOrderSource::System,
                    .contextPlayer = owner->player,
                    .issuedTick = event.confirmedTick,
                    .sourceSequence = 1,
                    .systemPurpose =
                        ObjectOrderSystemPurpose::ParachuteLanding,
                });
                ++queue->revision;
            } else {
                const std::optional<ecs::entity> transport =
                    m_world.m_objects.entityFromIdIncludingPending(
                        event.parachuteLandingTransport);
                const ObjectProducerComponent* producer = transport
                    ? ecs::try_get<ObjectProducerComponent>(
                          m_world.m_registry, *transport)
                    : nullptr;
                const std::optional<ObjectProductionExitRoute> route =
                    producer && producer->producer
                    ? m_world.m_objectProduction.spawnRallyRoute(
                          m_world.m_registry, m_world.m_objects,
                          producer->producer, owner->player)
                    : std::nullopt;
                if (route) {
                    production_exit::queueProductionExitRoute(
                        m_world.m_registry, *rider, *route,
                        event.confirmedTick, m_content.m_navigation);
                }
                // No route is the explicit aiIdle(CMD_FROM_AI) branch. The
                // queue was already cleared and revision-bumped above.
            }
        }
    }

    if (event.kind == ObjectContainmentRequestKind::Detach &&
        event.exposeStealthUnits && event.object) {
        const std::optional<ecs::entity> passenger =
            m_world.m_objects
                .entityFromIdIncludingPending(event.object);
        const ObjectKindOfComponent* kinds = passenger
            ? ecs::try_get<ObjectKindOfComponent>(
                  m_world.m_registry,
                  *passenger)
            : nullptr;
        if (kinds && game::objectHasKind(
                         kinds->mask,
                         game::ObjectKindOf::StealthGarrison)) {
            static_cast<void>(m_world
                .m_objectSimulation.markObjectDetected(
                    m_world.m_registry,
                    m_world.m_objects,
                    event.object, 0, event.confirmedTick));
        }
    }

    const std::optional<ecs::entity> building =
        m_world.m_objects
            .entityFromIdIncludingPending(event.container);
    if (!building) return finish();
    ObjectContainmentRuntimeComponent* containment =
        ecs::try_get<ObjectContainmentRuntimeComponent>(
            m_world.m_registry, *building);
    if (!containment || !containment->plan) return finish();

    // RefCode's TTAUDIO_soundEnter/soundExit belong to the CONTAINER, not the
    // passenger: the comments read "Sound when another unit enters me" and
    // "...exits me". So a bunker plays its own authored cue as squads move
    // through it, and the infantry stay silent. Emitted here, before the
    // cave/garrison ownership branches take their early returns, so every
    // accepted attach and detach is audible regardless of containment kind.
    if (const ThingTemplateComponent* buildingType =
            ecs::try_get<ThingTemplateComponent>(
                m_world.m_registry, *building);
        buildingType && buildingType->archetype) {
        const game::ThingTemplate& buildingTemplate =
            buildingType->archetype->templateData;
        const container::String& containerCue =
            event.kind == ObjectContainmentRequestKind::Attach
                ? buildingTemplate.soundEnter
                : buildingTemplate.soundExit;
        if (!containerCue.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = containerCue,
                .emitter = event.container,
                .owner = event.container,
            }));
        }
    }

    // OpenContain owns a second, module-authored EnterSound/ExitSound pair in
    // addition to ThingTemplate SoundEnter/SoundExit above.  RefCode suppresses
    // repeated module cues from the same container in one logic frame (bulk
    // unload is the common case), while the object-level onContaining /
    // onRemoving cue remains per passenger.
    if (RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(m_world.m_registry, *building)) {
        uint64_t& lastTick = event.kind == ObjectContainmentRequestKind::Attach
            ? visual->lastContainmentEnterAudioTick
            : visual->lastContainmentExitAudioTick;
        if (lastTick != event.confirmedTick) {
            const auto authoredCue = std::find_if(
                containment->plan->rules.begin(),
                containment->plan->rules.end(),
                [&](const ObjectContainmentRule& rule) {
                    return event.kind == ObjectContainmentRequestKind::Attach
                        ? !rule.enterSound.empty() : !rule.exitSound.empty();
                });
            if (authoredCue != containment->plan->rules.end()) {
                const container::String& cue =
                    event.kind == ObjectContainmentRequestKind::Attach
                        ? authoredCue->enterSound : authoredCue->exitSound;
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = cue,
                    .emitter = event.container,
                    .owner = event.container,
                }));
            }
            lastTick = event.confirmedTick;
        }
    }

    // Despite its authored name, SoundFallingFromPlane is the passenger-side
    // OpenContain removal cue in RefCode.  It is emitted for every accepted
    // detach, immediately after the container's SoundExit, and is not tied to
    // parachute state or to a particular transport implementation.
    if (event.kind == ObjectContainmentRequestKind::Detach && event.object) {
        const std::optional<ecs::entity> passenger =
            m_world.m_objects.entityFromIdIncludingPending(event.object);
        const ThingTemplateComponent* passengerType = passenger
            ? ecs::try_get<ThingTemplateComponent>(
                  m_world.m_registry, *passenger)
            : nullptr;
        if (passengerType && passengerType->archetype) {
            const container::String& passengerCue =
                passengerType->archetype->templateData.soundFallingFromPlane;
            if (!passengerCue.empty()) {
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = passengerCue,
                    .emitter = event.object,
                    .owner = event.object,
                }));
            }
        }
    }

    const bool caveEvent = containment->hasCave && std::any_of(
        containment->plan->rules.begin(), containment->plan->rules.end(),
        [](const ObjectContainmentRule& rule) {
            return rule.kind == ObjectContainmentKind::Cave;
        });
    const bool garrisonEvent = std::any_of(
        containment->plan->rules.begin(), containment->plan->rules.end(),
        [](const ObjectContainmentRule& rule) {
            return rule.kind == ObjectContainmentKind::Garrison;
        });
    // Read while containment is known live: the Attach suffix below publishes
    // ownership changes that can move the component pool this points into.
    const bool openContainment = std::any_of(
        containment->plan->rules.begin(), containment->plan->rules.end(),
        [](const ObjectContainmentRule& rule) {
            return rule.kind == ObjectContainmentKind::Open;
        });

    if (event.kind == ObjectContainmentRequestKind::Detach &&
        garrisonEvent) {
        const ObjectContainmentComponent* currentContents =
            ecs::try_get<ObjectContainmentComponent>(
                m_world.m_registry, *building);
        if ((!currentContents || currentContents->objects.empty()) &&
            containment->garrisonHasOriginalOwnership) {
            ObjectTeamId restoreTeam = containment->garrisonOriginalTeam;
            if (!restoreTeam ||
                !m_world.m_objectTeams.isOwnedBy(
                    restoreTeam, containment->garrisonOriginalOwner)) {
                restoreTeam = m_world
                    .m_objectTeams.defaultTeam(
                        containment->garrisonOriginalOwner)
                    .value_or(INVALID_OBJECT_TEAM_ID);
            }
            if (restoreTeam) {
                static_cast<void>(m_ownership.transferObjectToTeam(
                    event.container, restoreTeam, event.confirmedTick));
            }
            containment->garrisonOriginalOwner = INVALID_PLAYER_ID;
            containment->garrisonOriginalTeam = INVALID_OBJECT_TEAM_ID;
            containment->garrisonHasOriginalOwnership = false;
        }
        return finish();
    }

    // transferObjectToTeam emplaces components that cave-network iteration
    // reads (PrimaryTeamComponent among them) and publishes owner-changed
    // effects that can spawn objects, so an entrance set is snapshotted by
    // ObjectId and re-resolved before it is mutated, exactly as the sibling
    // handlers do. containment itself points into a pool that a transfer can
    // move, so the network identity is latched before any mutation as well.
    const auto caveEntranceRuntime =
        [this](ObjectId entrance) -> ObjectContainmentRuntimeComponent* {
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(entrance);
        return entity
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                  m_world.m_registry, *entity)
            : nullptr;
    };
    const auto caveNetworkEntrances = [this](int32_t caveIndex) {
        container::Vector<ObjectId> entrances;
        const auto view = ecs::view<
            const ObjectIdentityComponent,
            const ObjectContainmentRuntimeComponent>(
                m_world.m_registry);
        entrances.reserve(view.size_hint());
        for (const ecs::entity entrance : view) {
            const ObjectContainmentRuntimeComponent& runtime =
                view.template get<
                    const ObjectContainmentRuntimeComponent>(entrance);
            const ObjectId id = view.template get<
                const ObjectIdentityComponent>(entrance).id;
            if (!id || !runtime.hasCave || runtime.caveIndex != caveIndex) {
                continue;
            }
            entrances.push_back(id);
        }
        std::sort(entrances.begin(), entrances.end());
        return entrances;
    };
    const auto caveNetworkOccupantCount =
        [this, &caveNetworkEntrances](int32_t caveIndex) {
        size_t count = 0;
        for (const ObjectId entrance : caveNetworkEntrances(caveIndex)) {
            const std::optional<ecs::entity> entranceEntity =
                m_world.m_objects.entityFromIdIncludingPending(entrance);
            const ObjectContainmentRuntimeComponent* runtime = entranceEntity
                ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                      m_world.m_registry, *entranceEntity)
                : nullptr;
            const ObjectContainmentComponent* contents = entranceEntity
                ? ecs::try_get<ObjectContainmentComponent>(
                      m_world.m_registry, *entranceEntity)
                : nullptr;
            if (!runtime || !runtime->plan || !contents) continue;
            for (const ObjectContainedObjectRecord& record :
                 contents->objects) {
                const std::optional<ecs::entity> passenger =
                    m_world.m_objects.entityFromIdIncludingPending(
                        record.object);
                const ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(
                          m_world.m_registry, *passenger)
                    : nullptr;
                if (!edge || edge->container != entrance ||
                    edge->containmentRuleIndex >= runtime->plan->rules.size() ||
                    runtime->plan->rules[edge->containmentRuleIndex].kind !=
                        ObjectContainmentKind::Cave) {
                    continue;
                }
                ++count;
            }
        }
        return count;
    };

    if (event.kind == ObjectContainmentRequestKind::Detach && caveEvent) {
        if (caveNetworkOccupantCount(containment->caveIndex) == 0) {
            const int32_t caveIndex = containment->caveIndex;
            for (const ObjectId entrance : caveNetworkEntrances(caveIndex)) {
                ObjectContainmentRuntimeComponent* runtime =
                    caveEntranceRuntime(entrance);
                if (!runtime || !runtime->hasCave ||
                    runtime->caveIndex != caveIndex ||
                    !runtime->caveHasOriginalOwnership) {
                    continue;
                }
                const PlayerId originalOwner = runtime->caveOriginalOwner;
                ObjectTeamId restoreTeam = runtime->caveOriginalTeam;
                if (!restoreTeam ||
                    !m_world.m_objectTeams
                         .isOwnedBy(restoreTeam, originalOwner)) {
                    restoreTeam = m_world
                        .m_objectTeams.defaultTeam(originalOwner)
                        .value_or(INVALID_OBJECT_TEAM_ID);
                }
                if (restoreTeam) {
                    static_cast<void>(m_ownership.transferObjectToTeam(
                        entrance, restoreTeam, event.confirmedTick));
                    runtime = caveEntranceRuntime(entrance);
                    if (!runtime) continue;
                }
                runtime->caveOriginalOwner = INVALID_PLAYER_ID;
                runtime->caveOriginalTeam = INVALID_OBJECT_TEAM_ID;
                runtime->caveHasOriginalOwnership = false;
            }
        }
        return finish();
    }

    if (event.kind != ObjectContainmentRequestKind::Attach) return finish();
    const std::optional<ecs::entity> entrant =
        m_world.m_objects
            .entityFromIdIncludingPending(event.object);
    if (!entrant) return finish();
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
        m_world.m_registry, *entrant);
    const ObjectContainedByComponent* edge =
        ecs::try_get<ObjectContainedByComponent>(
            m_world.m_registry, *entrant);
    if (!owner || !edge || edge->container != event.container) {
        return finish();
    }
    // Same reason as openContainment: the entrant's OwnerComponent may be
    // relocated by the ownership publications this suffix performs.
    const PlayerId entrantOwner = owner->player;
    const ObjectContainmentRule* admittedRule =
        edge->containmentRuleIndex < containment->plan->rules.size()
        ? &containment->plan->rules[edge->containmentRuleIndex]
        : nullptr;

    if (admittedRule &&
        admittedRule->kind == ObjectContainmentKind::Cave &&
        entrantOwner) {
        const size_t networkOccupantCount =
            caveNetworkOccupantCount(containment->caveIndex);
        if (networkOccupantCount != 1) return finish();
        const std::optional<ObjectTeamId> entrantTeam =
            m_world.m_objectTeams.defaultTeam(entrantOwner);
        const int32_t caveIndex = containment->caveIndex;
        for (const ObjectId entrance : caveNetworkEntrances(caveIndex)) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromIdIncludingPending(entrance);
            ObjectContainmentRuntimeComponent* runtime = entity
                ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            const OwnerComponent* entranceOwner = entity
                ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
                : nullptr;
            const PrimaryTeamComponent* entranceTeam = entity
                ? ecs::try_get<PrimaryTeamComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            if (!runtime || !entranceOwner || !entranceTeam ||
                !runtime->hasCave || runtime->caveIndex != caveIndex) {
                continue;
            }
            if (!runtime->caveHasOriginalOwnership) {
                runtime->caveOriginalOwner = entranceOwner->player;
                runtime->caveOriginalTeam = entranceTeam->team;
                runtime->caveHasOriginalOwnership = true;
            }
            if (entrantTeam) {
                static_cast<void>(m_ownership.transferObjectToTeam(
                    entrance, *entrantTeam, event.confirmedTick));
            }
        }
    } else if (admittedRule &&
               admittedRule->kind == ObjectContainmentKind::Garrison) {
        const OwnerComponent* buildingOwner = ecs::try_get<OwnerComponent>(
            m_world.m_registry, *building);
        const PrimaryTeamComponent* buildingTeam =
            ecs::try_get<PrimaryTeamComponent>(
                m_world.m_registry, *building);
        if (!containment->garrisonHasOriginalOwnership && buildingOwner &&
            buildingTeam) {
            containment->garrisonOriginalOwner = buildingOwner->player;
            containment->garrisonOriginalTeam = buildingTeam->team;
            containment->garrisonHasOriginalOwnership = true;
        }
        if (const std::optional<ObjectTeamId> entrantTeam =
                m_world.m_objectTeams.defaultTeam(entrantOwner)) {
            static_cast<void>(m_ownership.transferObjectToTeam(
                event.container, *entrantTeam, event.confirmedTick));
        }
        static_cast<void>(m_content.m_players
            .recordAcademyEvent(
                entrantOwner,
                PlayerAcademyEvent::BuildingGarrisoned));
    } else if (admittedRule &&
               admittedRule->kind == ObjectContainmentKind::Tunnel) {
        static_cast<void>(m_content.m_players
            .recordAcademyEvent(
                entrantOwner,
                PlayerAcademyEvent::UnitEnteredTunnelNetwork));
    }

    if (openContainment) {
        static_cast<void>(m_presentation
            .m_scriptGameplayEvents.recordBuildingEntered({
                .building = event.container,
                .player = entrantOwner,
                .confirmedTick = event.confirmedTick,
            }));
    }
    return finish();
}

bool GameSessionWeaponEventDrain::handleVehicleNeutralization(WorkItem item) {
    const ObjectVehicleNeutralizationRequest& event =
        item.vehicleNeutralization;
    static_cast<void>(m_content.m_players
        .recordAcademyEvent(
            NEUTRAL_PLAYER_ID,
            PlayerAcademyEvent::VehicleSniped));
    static_cast<void>(changeObjectOwner(
        event.target, NEUTRAL_PLAYER_ID, event.confirmedTick));
    closeCurrentReaction();
    return true;
}

bool GameSessionWeaponEventDrain::handleCratePickupBatch(WorkItem item) {
    applyCratePickupCommands(
        std::move(item.cratePickupBatch.commands));
    closeCurrentReaction();
    return true;
}

bool GameSessionWeaponEventDrain::handleCountermeasureFlareSpawn(
    WorkItem item) {
    const ObjectCountermeasureFlareSpawnCommand& command =
        item.countermeasureFlareSpawn;
    bool created = false;
    ObjectId flare = INVALID_OBJECT_ID;
    const std::optional<ecs::entity> source =
        m_world.m_objects.entityFromId(
            command.source);
    const OwnerComponent* owner = source
        ? ecs::try_get<OwnerComponent>(
              m_world.m_registry, *source)
        : nullptr;
    const PrimaryTeamComponent* team = source
        ? ecs::try_get<PrimaryTeamComponent>(
              m_world.m_registry, *source)
        : nullptr;
    if (source && owner && owner->player && team && team->team &&
        !command.flareTemplate.empty()) {
        const GameSessionObjectSpawnResult spawned = m_lifecycle.spawnObject({
            .templateName = command.flareTemplate,
            .owner = owner->player,
            .primaryTeam = team->team,
            .transform = ObjectFixedTransformComponent{
                .position = command.position,
                .yawRadians = command.orientationRadians,
                .authoritative = true,
            },
            .origin = ObjectCreationOrigin::System,
            .confirmedTick = command.confirmedTick,
            .producer = command.source,
        });
        closeCurrentReaction();
        if (spawned) {
            ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(
                    m_world.m_registry,
                    *spawned.entity);
            if (physics) {
                physics->velocityUnitsPerSecond =
                    command.inheritedVelocityUnitsPerSecond;
                physics->pendingForce.x += command.motiveForce.x;
                physics->pendingForce.y += command.motiveForce.y;
                physics->pendingForce.z += command.motiveForce.z;
                flare = spawned.object;
                created = true;
            } else {
                static_cast<void>(m_lifecycle.requestDestroyObject(
                    spawned.object, ObjectDestroyReason::System,
                    command.confirmedTick));
                closeCurrentReaction();
            }
        }
    }
    m_world.m_objectSimulation
        .acknowledgeCountermeasureFlareSpawn(
            m_world.m_registry,
            m_world.m_objects,
            command.source, command.ruleIndex, flare, created,
            command.confirmedTick);
    closeCurrentReaction();
    return true;
}

} // namespace engine::detail
