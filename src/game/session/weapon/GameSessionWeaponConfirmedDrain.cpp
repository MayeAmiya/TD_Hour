#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/transaction/GameSessionProductionExitRoutes.h"

#include "debug/debug.h"
#include "core/container/string_utils.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationFootprintRasterizer.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectFloat.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/lifecycle/ObjectLifetime.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace engine::detail {
namespace {

[[nodiscard]] ObjectId transportProducer(
    const ObjectTransportGameplayPayload& payload) noexcept {
    return std::visit([](const auto& event) -> ObjectId {
        using Event = std::decay_t<decltype(event)>;
        if constexpr (std::is_same_v<
                          Event, ObjectTransportPayloadStrafeTransaction> ||
                      std::is_same_v<
                          Event, ObjectTransportPayloadWeaponTransaction> ||
                      std::is_same_v<
                          Event, ObjectTransportPayloadFinishedTransaction>) {
            return event.transport;
        } else if constexpr (std::is_same_v<
                                 Event, ObjectTransportOclTransaction> ||
                             std::is_same_v<
                                 Event,
                                 ObjectTransportWeaponAtPositionTransaction> ||
                             std::is_same_v<
                                 Event,
                                 ObjectTransportBunkerBustTransaction>) {
            return event.source;
        } else if constexpr (std::is_same_v<
                                 Event,
                                 ObjectTransportVeterancySyncTransaction>) {
            return event.lower;
        } else if constexpr (std::is_same_v<
                                 Event,
                                 ObjectTransportHijackerReleaseTransaction>) {
            return event.hijacker;
        } else if constexpr (std::is_same_v<
                                 Event,
                                 ObjectTransportBattleBusStartTransaction> ||
                             std::is_same_v<
                                 Event,
                                 ObjectTransportBattleBusLandedTransaction>) {
            return event.battleBus;
        } else {
            return event.object;
        }
    }, payload);
}

} // namespace

bool GameSessionWeaponEventDrain::commandOrder(
    const ObjectSystemWeaponFireCommand& left,
    const ObjectSystemWeaponFireCommand& right) noexcept {
    if (left.emissionSequence != right.emissionSequence) {
        return left.emissionSequence < right.emissionSequence;
    }
    if (left.source != right.source) return left.source < right.source;
    if (left.authoredOrder != right.authoredOrder) {
        return left.authoredOrder < right.authoredOrder;
    }
    return left.sourceShotSequence < right.sourceShotSequence;
}

uint64_t GameSessionWeaponEventDrain::workSequence(
    const WorkItem& item) noexcept {
    if (item.admissionOrdinal != 0) return item.admissionOrdinal;
    switch (item.kind) {
    case WorkKind::Weapon:
    case WorkKind::WeaponImpact:
        return item.weapon.emissionSequence;
    case WorkKind::ObjectCreationList: return item.ocl.emissionSequence;
    case WorkKind::Crate: return item.crate.emissionSequence;
    case WorkKind::Replacement: return item.replacement.emissionSequence;
    case WorkKind::UpgradeFx: return item.upgradeFx.emissionSequence;
    case WorkKind::StructureFx: return item.structureFx.emissionSequence;
    case WorkKind::TransitionOcl:
        return item.transitionOcl.emissionSequence;
    case WorkKind::InstantDeath:
        return item.instantDeath.fxEmissionSequence;
    case WorkKind::SlowDeath:
        return item.slowDeath.fxEmissionSequence;
    case WorkKind::SlowDeathRubble:
        return item.slowDeathRubble.emissionSequence;
    case WorkKind::TopplePathfind:
        return item.topplePathfind.emissionSequence;
    case WorkKind::ToppleStump:
        return item.toppleStump.emissionSequence;
    case WorkKind::PhysicsCrash:
        return item.physicsCrash.submissionOrdinal;
    case WorkKind::AIMovementObstructionBatch:
        return item.aiMovementObstructionBatch.submissionOrdinal;
    case WorkKind::DestroyObject:
        return item.destroyObject.emissionSequence;
    case WorkKind::MineSpawn:
        return item.mineSpawn.emissionSequence;
    case WorkKind::ParticleUplinkRemnant:
        return item.particleUplinkRemnant.emissionSequence;
    case WorkKind::WaveBridgeImpact:
        return item.waveBridgeImpact.emissionSequence;
    case WorkKind::CheckpointNavigation:
        return item.checkpointNavigation.submissionOrdinal;
    case WorkKind::TensileNavigation:
        return item.tensileNavigation.submissionOrdinal;
    case WorkKind::DynamicGeometry:
        return item.dynamicGeometry.submissionOrdinal;
    case WorkKind::Transport:
        return item.transport.submissionOrdinal;
    case WorkKind::DeathWalk:
        return item.admissionOrdinal;
    case WorkKind::DeleteWalk:
        return item.deleteWalk.submissionOrdinal;
    case WorkKind::BodyResume:
        return item.admissionOrdinal;
    case WorkKind::OwnershipChange:
        return item.ownershipChange.submissionOrdinal;
    case WorkKind::Defection:
        return item.defection.submissionOrdinal;
    case WorkKind::PilotVehicleTakeover:
        return item.pilotVehicleTakeover.submissionOrdinal;
    case WorkKind::RailedTransportDockAttach:
        return item.railedTransportDockAttach.request.submissionOrdinal;
    case WorkKind::RailroadDisembark:
        return item.railroadDisembark.submissionOrdinal;
    case WorkKind::RailroadCarriageSpawn:
        return item.railroadCarriageSpawn.submissionOrdinal;
    case WorkKind::SpawnSlave:
        return item.spawnSlave.submissionOrdinal;
    case WorkKind::SpecialPowerSpawn:
        return item.specialPowerSpawn.emissionSequence;
    case WorkKind::BridgeState:
        return item.bridgeState.submissionOrdinal;
    case WorkKind::ConstructionCompletion:
        return item.constructionCompletion.submissionOrdinal;
    case WorkKind::BridgeRepairScaffoldBatch:
        return item.bridgeRepairScaffoldBatch.submissionOrdinal;
    case WorkKind::RebuildTargetRemap:
        return item.rebuildTargetRemap.submissionOrdinal;
    case WorkKind::RebuildHoleExpose:
        return item.rebuildHoleExpose.submissionOrdinal;
    case WorkKind::RebuildWorkerSpawn:
        return item.rebuildWorkerSpawn.submissionOrdinal;
    case WorkKind::RebuildCompletion:
        return item.rebuildCompletion.submissionOrdinal;
    case WorkKind::ContainmentEvent:
        return item.containmentEvent.submissionOrdinal;
    case WorkKind::VehicleNeutralization:
        return item.vehicleNeutralization.submissionOrdinal;
    case WorkKind::CratePickupBatch:
        return item.cratePickupBatch.submissionOrdinal;
    case WorkKind::CountermeasureFlareSpawn:
        return item.countermeasureFlareSpawn.submissionOrdinal;
    case WorkKind::ProductionSpawn:
        return item.productionSpawn.submissionOrdinal;
    case WorkKind::ProductionUpgrade:
        return item.productionUpgrade.submissionOrdinal;
    case WorkKind::SpecialAbilityEffect:
        return item.specialAbilityEffect.submissionOrdinal;
    case WorkKind::SpecialPowerCompletion:
        return item.specialPowerCompletion.submissionOrdinal;
    case WorkKind::Damage: return item.admissionOrdinal;
    case WorkKind::Count: break;
    }
    return uint64_t{0};
}

ObjectId GameSessionWeaponEventDrain::workSource(
    const WorkItem& item) noexcept {
    switch (item.kind) {
    case WorkKind::Weapon:
    case WorkKind::WeaponImpact:
        return item.weapon.source;
    case WorkKind::ObjectCreationList: return item.ocl.source;
    case WorkKind::Crate: return item.crate.object;
    case WorkKind::Replacement: return item.replacement.source;
    case WorkKind::UpgradeFx: return item.upgradeFx.source;
    case WorkKind::StructureFx: return item.structureFx.object;
    case WorkKind::TransitionOcl: return item.transitionOcl.object;
    case WorkKind::InstantDeath: return item.instantDeath.object;
    case WorkKind::SlowDeath: return item.slowDeath.object;
    case WorkKind::SlowDeathRubble:
        return item.slowDeathRubble.source;
    case WorkKind::TopplePathfind: return item.topplePathfind.object;
    case WorkKind::ToppleStump: return item.toppleStump.source;
    case WorkKind::PhysicsCrash: return item.physicsCrash.source;
    case WorkKind::AIMovementObstructionBatch:
        return item.aiMovementObstructionBatch.events.empty()
            ? INVALID_OBJECT_ID
            : item.aiMovementObstructionBatch.events.front().mover.object;
    case WorkKind::DestroyObject: return item.destroyObject.source;
    case WorkKind::MineSpawn: return item.mineSpawn.producer;
    case WorkKind::ParticleUplinkRemnant:
        return item.particleUplinkRemnant.source;
    case WorkKind::WaveBridgeImpact:
        return item.waveBridgeImpact.source;
    case WorkKind::CheckpointNavigation:
        return item.checkpointNavigation.object;
    case WorkKind::TensileNavigation:
        return item.tensileNavigation.object;
    case WorkKind::DynamicGeometry:
        return item.dynamicGeometry.object;
    case WorkKind::Transport:
        return transportProducer(item.transport.payload);
    case WorkKind::DeathWalk:
        return item.deathWalk.damage.target;
    case WorkKind::DeleteWalk:
        return item.deleteWalk.object;
    case WorkKind::BodyResume:
        return item.bodyResume.damage.target;
    case WorkKind::OwnershipChange:
        return item.ownershipChange.object;
    case WorkKind::Defection:
        return item.defection.source;
    case WorkKind::PilotVehicleTakeover:
        return item.pilotVehicleTakeover.pilot;
    case WorkKind::RailedTransportDockAttach:
        return item.railedTransportDockAttach.request.container;
    case WorkKind::RailroadDisembark:
        return item.railroadDisembark.carriage;
    case WorkKind::RailroadCarriageSpawn:
        return item.railroadCarriageSpawn.locomotive;
    case WorkKind::SpawnSlave:
        return item.spawnSlave.spawner;
    case WorkKind::SpecialPowerSpawn:
        return item.specialPowerSpawn.source;
    case WorkKind::BridgeState:
        return item.bridgeState.object;
    case WorkKind::ConstructionCompletion:
        return item.constructionCompletion.object;
    case WorkKind::BridgeRepairScaffoldBatch:
        return item.bridgeRepairScaffoldBatch.intents.empty()
            ? INVALID_OBJECT_ID
            : item.bridgeRepairScaffoldBatch.intents.front().bridge;
    case WorkKind::RebuildTargetRemap:
        return item.rebuildTargetRemap.from;
    case WorkKind::RebuildHoleExpose:
        return item.rebuildHoleExpose.source;
    case WorkKind::RebuildWorkerSpawn:
        return item.rebuildWorkerSpawn.hole;
    case WorkKind::RebuildCompletion:
        return item.rebuildCompletion.hole;
    case WorkKind::ContainmentEvent:
        return item.containmentEvent.container;
    case WorkKind::VehicleNeutralization:
        return item.vehicleNeutralization.source;
    case WorkKind::CratePickupBatch:
        return item.cratePickupBatch.commands.empty()
            ? INVALID_OBJECT_ID
            : item.cratePickupBatch.commands.front().crate;
    case WorkKind::CountermeasureFlareSpawn:
        return item.countermeasureFlareSpawn.source;
    case WorkKind::ProductionSpawn:
        return item.productionSpawn.producer;
    case WorkKind::ProductionUpgrade:
        return item.productionUpgrade.producer;
    case WorkKind::SpecialAbilityEffect:
        return item.specialAbilityEffect.source;
    case WorkKind::SpecialPowerCompletion:
        return item.specialPowerCompletion.object;
    case WorkKind::Damage: return item.damage.source;
    case WorkKind::Count: break;
    }
    return INVALID_OBJECT_ID;
}

gameplay::GameplayEnvelope GameSessionWeaponEventDrain::workEnvelope(
    const WorkItem& item) const noexcept {
    return {
        .confirmedTick =
            m_presentation.m_confirmedTick,
        // Every detached producer stream carries the same owner-thread
        // admission ordinal. Private authored/source ordinals stay inside
        // their parent transaction and never enter this comparator.
        .submissionOrdinal = workSequence(item),
        .producer = workSource(item),
    };
}

void GameSessionWeaponEventDrain::appendCreateObjectWork(
    container::Vector<WorkItem>& output,
    ObjectCreateObjectDieEvent source) {
    const game::ObjectCreationListContentId content =
        m_content.m_contentSnapshot.findObjectCreationListId(
            source.objectCreationList);
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromIdIncludingPending(source.object);
    if (!content || !entity) return;
    const OwnerComponent* ownerComponent =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    const PrimaryTeamComponent* teamComponent =
        ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *entity);
    const PlayerId owner = ownerComponent
        ? ownerComponent->player : INVALID_PLAYER_ID;
    const ObjectTeamId team = teamComponent ? teamComponent->team
        : m_world.m_objectTeams.defaultTeam(owner).value_or(
              INVALID_OBJECT_TEAM_ID);
    if (!owner || !team) return;
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
    LogicFixedVec3 position = transform
        ? readAuthoritativeObjectPosition(
              m_world.m_registry,
              *entity, *transform)
        : LogicFixedVec3{};
    ObjectPhysicsComponent::Scalar rotation = transform
        ? readAuthoritativeObjectYaw(
              m_world.m_registry,
              *entity, *transform)
        : ObjectPhysicsComponent::Scalar{};
    ObjectPhysicsComponent::Scalar pitch{};
    ObjectPhysicsComponent::Scalar roll{};
    bool ownsFullAttitude = false;
    LogicFixedVec3 velocity;
    if (const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(m_world.m_registry, *entity)) {
        velocity = physics->velocityUnitsPerSecond;
        if (physics->ownsAttitude) {
            rotation = physics->yaw;
            pitch = physics->pitch;
            roll = physics->roll;
            ownsFullAttitude = true;
        }
    }
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    if (const ObjectVeterancyComponent* state =
            ecs::try_get<ObjectVeterancyComponent>(m_world.m_registry, *entity)) {
        veterancy = state->level;
    }
    ObjectCreationListCompletion completion;
    if (source.kind == ObjectDeathOclEventKind::CreateObject) {
        completion = {
            .kind = ObjectCreationListCompletionKind::CreateObjectDie,
            .previousHealth = source.previousHealth,
            .maximumHealth = source.maximumHealth,
            .subdualDamage = source.subdualDamage,
            .damageSource = source.damageSource,
            .transferPreviousHealth = source.transferPreviousHealth,
            .transferSelection = source.transferSelection,
        };
    }
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(m_world.m_registry, *entity);
    output.push_back({
        .kind = WorkKind::ObjectCreationList,
        .ocl = {
            .content = content,
            .source = source.object,
            .owner = owner,
            .primaryTeam = team,
            .primaryPosition = position,
            .sourceVelocity = velocity,
            .orientationRadians = rotation,
            .pitchRadians = pitch,
            .rollRadians = roll,
            .veterancy = veterancy,
            .authoredOrder = source.authoredOrder,
            .emissionSequence = source.emissionSequence,
            .confirmedTick = source.confirmedTick,
            .sourcePathfindLayer = source.sourcePathfindLayer,
            .sourceAirborne = airborne && airborne->isAirborne,
            .sourceOwnsFullAttitude = ownsFullAttitude,
            .completion = completion,
        },
        .oclState = std::make_shared<OclWorkState>(),
    });
    if (source.kind == ObjectDeathOclEventKind::EjectPilot) {
        const std::optional<game::FxInvocationAnchor> anchor =
            session_fx::snapshotAnchor(
                m_world.m_registry, m_world.m_objects, source.object);
        const std::optional<math::vec3> audioPosition = anchor
            ? std::optional<math::vec3>{anchor->position}
            : std::nullopt;
        if (!source.voiceEject.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = source.voiceEject,
                .emitter = source.object,
                .owner = source.object,
                .position = audioPosition,
            }));
        }
        if (!source.soundEject.empty()) {
            static_cast<void>(m_publication.emitAudioEvent({
                .eventName = source.soundEject,
                .emitter = source.object,
                .owner = source.object,
                .position = audioPosition,
            }));
        }
    }
}

void GameSessionWeaponEventDrain::pushCommands(
    container::Vector<ObjectSystemWeaponFireCommand> commands) {
    std::stable_sort(commands.begin(), commands.end(), commandOrder);
    container::Vector<WorkItem> pending;
    pending.reserve(commands.size());
    for (ObjectSystemWeaponFireCommand& command : commands) {
        pending.push_back({
            .kind = WorkKind::Weapon,
            .weapon = std::move(command),
        });
    }
    pushPendingWork(std::move(pending));
}

void GameSessionWeaponEventDrain::collectPendingWork(
    container::Vector<WorkItem>& output) {
    for (ObjectDeleteWalkState& walk :
         m_world.m_objectSimulation
             .leaseObjectDeleteWalks()) {
        const size_t remainingEntries = walk.plan &&
                walk.nextEntryIndex < walk.plan->entries.size()
            ? walk.plan->entries.size() - walk.nextEntryIndex
            : 0;
        const size_t structuralReservation = remainingEntries + 1u;
        if (m_reservedStructuralTransactions >
            kMaximumGameplayTransactions -
                std::min(structuralReservation,
                         kMaximumGameplayTransactions)) {
            m_reservedStructuralTransactions =
                kMaximumGameplayTransactions + 1u;
        } else {
            m_reservedStructuralTransactions += structuralReservation;
        }
        output.push_back({
            .kind = WorkKind::DeleteWalk,
            .deleteWalk = std::move(walk),
        });
    }
    for (ObjectDeleteDestroyRequest& request :
         m_world.m_objectSimulation
             .leaseObjectDeleteDestroyRequests()) {
        output.push_back({
            .kind = WorkKind::DestroyObject,
            .destroyObject = {
                .object = request.object,
                .reason = request.reason,
                .source = request.source,
                .emissionSequence = request.submissionOrdinal,
                .confirmedTick = request.confirmedTick,
            },
        });
    }
    // This stream is authoritative script input, not a presentation
    // diagnostic. Drain it at every re-entrant gameplay barrier so a
    // later ObjectSimulation update cannot discard a completion emitted
    // by a nested death or projectile launch.
    for (ObjectDamageTransactionIngress& ingress :
         m_world.m_objectSimulation
             .leaseReadyDamageTransactions(
                 m_presentation.m_confirmedTick)) {
        output.push_back({
            .kind = WorkKind::Damage,
            .admissionOrdinal = ingress.submissionOrdinal,
            .damage = std::move(ingress.request),
        });
    }
    for (ObjectOwnershipChangeRequest& request :
         m_world.m_objectSimulation
             .leaseOwnershipChangeRequests()) {
        output.push_back({
            .kind = WorkKind::OwnershipChange,
            .ownershipChange = std::move(request),
        });
    }
    for (ObjectDefectionRequest& request :
         m_world.m_objectSimulation
             .leaseObjectDefectionRequests()) {
        output.push_back({
            .kind = WorkKind::Defection,
            .defection = std::move(request),
        });
    }
    for (ObjectPilotVehicleTakeoverRequest& request :
         m_world.m_objectSimulation
             .leasePilotVehicleTakeoverRequests()) {
        output.push_back({
            .kind = WorkKind::PilotVehicleTakeover,
            .pilotVehicleTakeover = std::move(request),
        });
    }
    for (ObjectRailedTransportDockAttachCompletion& completion :
         m_world.m_objectSimulation
             .leaseRailedTransportDockAttachCompletions()) {
        output.push_back({
            .kind = WorkKind::RailedTransportDockAttach,
            .railedTransportDockAttach = std::move(completion),
        });
    }
    for (ObjectRailroadDisembarkRequest& request :
         m_world.m_objectSimulation
             .leaseRailroadDisembarkRequests()) {
        output.push_back({
            .kind = WorkKind::RailroadDisembark,
            .railroadDisembark = std::move(request),
        });
    }
    for (ObjectRailroadCarriageSpawnRequest& request :
         m_world.m_objectSimulation
             .leaseRailroadCarriageSpawnRequests()) {
        output.push_back({
            .kind = WorkKind::RailroadCarriageSpawn,
            .railroadCarriageSpawn = std::move(request),
        });
    }
    for (ObjectSpawnSlaveRequest& request :
         m_world.m_objectSimulation
             .leaseObjectSpawnSlaveRequests()) {
        output.push_back({
            .kind = WorkKind::SpawnSlave,
            .spawnSlave = std::move(request),
        });
    }
    for (ObjectSpecialPowerSpawnRequest& request :
         m_world.m_objectSimulation
             .leaseSpecialPowerSpawnRequests()) {
        output.push_back({
            .kind = WorkKind::SpecialPowerSpawn,
            .specialPowerSpawn = std::move(request),
        });
    }
    for (ObjectBridgeStateEvent& event :
         m_world.m_objectSimulation
             .leaseObjectBridgeStateEvents()) {
        output.push_back({
            .kind = WorkKind::BridgeState,
            .bridgeState = std::move(event),
        });
    }
    for (ObjectConstructionCompletionIntent& intent :
         m_world.m_objectSimulation
             .leaseCompletedObjectConstructions()) {
        output.push_back({
            .kind = WorkKind::ConstructionCompletion,
            .constructionCompletion = std::move(intent),
        });
    }
    {
        auto scaffoldLease = m_world
            .m_objectSimulation.leaseBridgeRepairScaffoldIntents();
        container::Vector<ObjectBridgeRepairScaffoldIntent>& scaffoldIntents =
            scaffoldLease.events();
        if (!scaffoldIntents.empty()) {
            BridgeRepairScaffoldBatchWork batch;
            batch.submissionOrdinal = std::numeric_limits<uint64_t>::max();
            batch.intents.reserve(scaffoldIntents.size());
            for (ObjectBridgeRepairScaffoldIntent& intent : scaffoldIntents) {
                if (intent.submissionOrdinal == 0) {
                    intent.submissionOrdinal = m_world.m_objectSimulation
                        .reserveGameplaySubmissionOrdinal();
                }
                batch.submissionOrdinal = std::min(
                    batch.submissionOrdinal, intent.submissionOrdinal);
                batch.intents.push_back(std::move(intent));
            }
            output.push_back({
                .kind = WorkKind::BridgeRepairScaffoldBatch,
                .bridgeRepairScaffoldBatch = std::move(batch),
            });
        }
    }
    for (ObjectRebuildTargetRemapIntent& intent :
         m_world.m_objectSimulation
             .leaseRebuildTargetRemapIntents()) {
        output.push_back({
            .kind = WorkKind::RebuildTargetRemap,
            .rebuildTargetRemap = std::move(intent),
        });
    }
    for (ObjectRebuildHoleExposeIntent& intent :
         m_world.m_objectSimulation
             .leaseRebuildHoleExposeIntents()) {
        output.push_back({
            .kind = WorkKind::RebuildHoleExpose,
            .rebuildHoleExpose = std::move(intent),
        });
    }
    for (ObjectRebuildWorkerSpawnIntent& intent :
         m_world.m_objectSimulation
             .leaseRebuildWorkerSpawnIntents()) {
        output.push_back({
            .kind = WorkKind::RebuildWorkerSpawn,
            .rebuildWorkerSpawn = std::move(intent),
        });
    }
    for (ObjectRebuildCompletionIntent& intent :
         m_world.m_objectSimulation
             .leaseRebuildCompletionIntents()) {
        output.push_back({
            .kind = WorkKind::RebuildCompletion,
            .rebuildCompletion = std::move(intent),
        });
    }
    for (ObjectContainmentEvent& event :
         m_world.m_objectSimulation
             .leaseContainmentEvents()) {
        output.push_back({
            .kind = WorkKind::ContainmentEvent,
            .containmentEvent = std::move(event),
        });
    }
    for (ObjectVehicleNeutralizationRequest& event :
         m_world.m_objectSimulation
             .leaseVehicleNeutralizationRequests()) {
        output.push_back({
            .kind = WorkKind::VehicleNeutralization,
            .vehicleNeutralization = std::move(event),
        });
    }
    {
        auto crateLease = m_world
            .m_objectSimulation.leaseCratePickupCommands();
        container::Vector<ObjectCratePickupCommand>& crateCommands =
            crateLease.events();
        for (ObjectCratePickupCommand& command : crateCommands) {
            if (command.submissionOrdinal == 0) {
                command.submissionOrdinal = m_world.m_objectSimulation
                    .reserveGameplaySubmissionOrdinal();
            }
        }
        // AllowMultiPickup and source destruction are one synchronous ZH
        // collide transaction per crate. Keep that source-local batch intact,
        // but do not pull unrelated crate collisions across other gameplay
        // ordinals.
        for (size_t begin = 0; begin < crateCommands.size();) {
            size_t end = begin + 1;
            while (end < crateCommands.size() &&
                   crateCommands[end].crate == crateCommands[begin].crate) {
                ++end;
            }
            CratePickupBatchWork batch;
            batch.submissionOrdinal = crateCommands[begin].submissionOrdinal;
            batch.commands.reserve(end - begin);
            for (size_t index = begin; index < end; ++index) {
                batch.submissionOrdinal = std::min(
                    batch.submissionOrdinal,
                    crateCommands[index].submissionOrdinal);
                batch.commands.push_back(std::move(crateCommands[index]));
            }
            output.push_back({
                .kind = WorkKind::CratePickupBatch,
                .cratePickupBatch = std::move(batch),
            });
            begin = end;
        }
    }
    for (ObjectCountermeasureFlareSpawnCommand& command :
         m_world.m_objectSimulation
             .leaseCountermeasureFlareSpawnCommands()) {
        output.push_back({
            .kind = WorkKind::CountermeasureFlareSpawn,
            .countermeasureFlareSpawn = std::move(command),
        });
    }
    for (ObjectProductionSpawnIntent& intent :
         m_world.m_pendingProductionSpawns) {
        output.push_back({
            .kind = WorkKind::ProductionSpawn,
            .productionSpawn = std::move(intent),
        });
    }
    m_world.m_pendingProductionSpawns.clear();
    for (ObjectProductionUpgradeCompletionIntent& intent :
         m_world.m_pendingProductionUpgrades) {
        output.push_back({
            .kind = WorkKind::ProductionUpgrade,
            .productionUpgrade = std::move(intent),
        });
    }
    m_world.m_pendingProductionUpgrades.clear();
    for (ObjectSpecialAbilityEffectRequest& effect :
         m_world.m_objectSimulation
             .leaseSpecialAbilityEffectRequests()) {
        output.push_back({
            .kind = WorkKind::SpecialAbilityEffect,
            .specialAbilityEffect = std::move(effect),
        });
    }
    for (ObjectSpecialPowerCompletionEvent& event :
         m_world.m_objectSimulation.leaseSpecialPowerCompletionEvents()) {
        output.push_back({
            .kind = WorkKind::SpecialPowerCompletion,
            .specialPowerCompletion = std::move(event),
        });
    }
    for (ObjectSystemWeaponFireCommand& command :
         m_world.m_objectSimulation.leaseSystemWeaponFireCommands()) {
        output.push_back({
            .kind = WorkKind::Weapon,
            .weapon = std::move(command),
        });
    }
    for (ObjectCreationListInvocation& invocation :
         m_world.m_objectSimulation.leaseObjectCreationListInvocations()) {
        output.push_back({
            .kind = WorkKind::ObjectCreationList,
            .ocl = std::move(invocation),
            .oclState = std::make_shared<OclWorkState>(),
        });
    }
    for (ObjectReplacementInvocation& invocation :
         m_world.m_objectSimulation.leaseObjectReplacementInvocations()) {
        output.push_back({
            .kind = WorkKind::Replacement,
            .replacement = std::move(invocation),
        });
    }
    for (ObjectUpgradeFxInvocation& invocation :
         m_world.m_objectSimulation.leaseObjectUpgradeFxInvocations()) {
        output.push_back({
            .kind = WorkKind::UpgradeFx,
            .upgradeFx = std::move(invocation),
        });
    }
    for (ObjectStructureEffectEvent& effect :
         m_world.m_objectSimulation.leaseStructureEffectEvents()) {
        if (effect.resource.empty()) continue;
        if (effect.kind == ObjectStructureEffectKind::FxList) {
            output.push_back({
                .kind = WorkKind::StructureFx,
                .structureFx = std::move(effect),
            });
            continue;
        }
        const game::ObjectCreationListContentId content =
            m_content.m_contentSnapshot.findObjectCreationListId(effect.resource);
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromIdIncludingPending(effect.object);
        if (!content || !entity) continue;
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
        const PrimaryTeamComponent* team =
            ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *entity);
        if (!owner || !team || !owner->player || !team->team) continue;
        game::ObjectVeterancyLevel veterancy =
            game::ObjectVeterancyLevel::Regular;
        if (const ObjectVeterancyComponent* state =
                ecs::try_get<ObjectVeterancyComponent>(
                    m_world.m_registry, *entity)) {
            veterancy = state->level;
        }
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(m_world.m_registry, *entity);
        output.push_back({
            .kind = WorkKind::ObjectCreationList,
            .ocl = {
                .content = content,
                .source = effect.object,
                .owner = owner->player,
                .primaryTeam = team->team,
                .primaryPosition = effect.position,
                .orientationRadians = effect.orientationRadians,
                .veterancy = veterancy,
                .authoredOrder = effect.authoredOrder,
                .emissionSequence = effect.emissionSequence,
                .confirmedTick = effect.confirmedTick,
                .sourcePathfindLayer = effect.sourcePathfindLayer,
                .sourceAirborne = airborne && airborne->isAirborne,
            },
            .oclState = std::make_shared<OclWorkState>(),
        });
    }
    for (ObjectCreateObjectDieEvent& source :
         m_world.m_objectSimulation.leaseCreateObjectDieEvents()) {
        appendCreateObjectWork(output, std::move(source));
    }
    for (ObjectCreateCrateDieEvent& source :
         m_world.m_objectSimulation.leaseCreateCrateDieEvents()) {
        output.push_back({
            .kind = WorkKind::Crate,
            .crate = std::move(source),
        });
    }
    for (ObjectTransitionDamageFxEvent& source :
         m_world.m_objectSimulation
             .leaseTransitionDamageGameplayEvents()) {
        output.push_back({
            .kind = WorkKind::TransitionOcl,
            .transitionOcl = std::move(source),
        });
    }
    for (ObjectInstantDeathEffectEvent& source :
         m_world.m_objectSimulation
             .leaseInstantDeathGameplayEvents()) {
        output.push_back({
            .kind = WorkKind::InstantDeath,
            .instantDeath = std::move(source),
        });
    }
    for (ObjectSlowDeathPhaseEvent& source :
         m_world.m_objectSimulation
             .leaseSlowDeathGameplayEvents()) {
        output.push_back({
            .kind = WorkKind::SlowDeath,
            .slowDeath = std::move(source),
        });
    }
    for (ObjectTopplePathfindRemovalRequest& source :
         m_world.m_objectSimulation
             .leaseTopplePathfindRemovalRequests()) {
        output.push_back({
            .kind = WorkKind::TopplePathfind,
            .topplePathfind = std::move(source),
        });
    }
    for (ObjectToppleStumpSpawnRequest& source :
         m_world.m_objectSimulation
             .leaseToppleStumpSpawnRequests()) {
        output.push_back({
            .kind = WorkKind::ToppleStump,
            .toppleStump = std::move(source),
        });
    }
    for (ObjectPhysicsCrashCommand& source :
         m_world.m_objectSimulation
             .leasePhysicsCrashCommands()) {
        output.push_back({
            .kind = WorkKind::PhysicsCrash,
            .physicsCrash = std::move(source),
        });
    }
    auto obstructionLease = m_world
        .m_objectSimulation.leaseAIMovementObstructionEvents();
    container::Vector<ObjectAIMovementObstructionEvent>& obstructions =
        obstructionLease.events();
    if (!obstructions.empty()) {
        // One physical contact pair is one synchronous ZH occurrence. Keep
        // its two directed blockedBy views together, but do not collapse the
        // complete physics pass under the first ordinal: unrelated gameplay
        // transactions must still interleave by their public submission
        // ordinal.
        std::sort(
            obstructions.begin(), obstructions.end(),
            [](const ObjectAIMovementObstructionEvent& left,
               const ObjectAIMovementObstructionEvent& right) {
                const ObjectId leftFirst = std::min(
                    left.mover.object, left.blocker.object);
                const ObjectId rightFirst = std::min(
                    right.mover.object, right.blocker.object);
                if (leftFirst != rightFirst) return leftFirst < rightFirst;
                const ObjectId leftSecond = std::max(
                    left.mover.object, left.blocker.object);
                const ObjectId rightSecond = std::max(
                    right.mover.object, right.blocker.object);
                if (leftSecond != rightSecond)
                    return leftSecond < rightSecond;
                if (left.blocker.object != right.blocker.object) {
                    return left.blocker.object < right.blocker.object;
                }
                return left.mover.object < right.mover.object;
            });
        size_t begin = 0;
        while (begin < obstructions.size()) {
            const ObjectId first = std::min(
                obstructions[begin].mover.object,
                obstructions[begin].blocker.object);
            const ObjectId second = std::max(
                obstructions[begin].mover.object,
                obstructions[begin].blocker.object);
            size_t end = begin + 1;
            while (end < obstructions.size() &&
                   std::min(obstructions[end].mover.object,
                            obstructions[end].blocker.object) == first &&
                   std::max(obstructions[end].mover.object,
                            obstructions[end].blocker.object) == second) {
                ++end;
            }
            AIMovementObstructionBatchWork batch;
            batch.events.reserve(end - begin);
            batch.submissionOrdinal =
                std::numeric_limits<uint64_t>::max();
            for (size_t index = begin; index < end; ++index) {
                batch.submissionOrdinal = std::min(
                    batch.submissionOrdinal,
                    obstructions[index].submissionOrdinal);
                batch.events.push_back(std::move(obstructions[index]));
            }
            output.push_back({
                .kind = WorkKind::AIMovementObstructionBatch,
                .aiMovementObstructionBatch = std::move(batch),
            });
            begin = end;
        }
    }
    for (ObjectMineSpawnCommand& source :
         m_world.m_objectSimulation
             .leaseMineSpawnCommands()) {
        output.push_back({
            .kind = WorkKind::MineSpawn,
            .mineSpawn = std::move(source),
        });
    }
    for (ObjectParticleUplinkRemnantSpawnRequest& source :
         m_world.m_objectSimulation
             .leaseParticleUplinkRemnantSpawnRequests()) {
        output.push_back({
            .kind = WorkKind::ParticleUplinkRemnant,
            .particleUplinkRemnant = std::move(source),
        });
    }
    for (ObjectWaveGuideBridgeImpact& source :
         m_world.m_objectSimulation
             .leaseWaveGuideBridgeImpacts()) {
        output.push_back({
            .kind = WorkKind::WaveBridgeImpact,
            .waveBridgeImpact = std::move(source),
        });
    }
    for (ObjectCheckpointNavigationEvent& source :
         m_world.m_objectSimulation
             .leaseCheckpointNavigationEvents()) {
        output.push_back({
            .kind = WorkKind::CheckpointNavigation,
            .checkpointNavigation = std::move(source),
        });
    }
    for (ObjectTensileFormationEvent& source :
         m_world.m_objectSimulation
             .leaseObjectTensileNavigationEvents()) {
        output.push_back({
            .kind = WorkKind::TensileNavigation,
            .tensileNavigation = std::move(source),
        });
    }
    for (ObjectDynamicGeometryGameplayEvent& source :
         m_world.m_objectSimulation
             .leaseDynamicGeometryGameplayEvents()) {
        output.push_back({
            .kind = WorkKind::DynamicGeometry,
            .dynamicGeometry = std::move(source),
        });
    }
    for (ObjectTransportGameplayTransaction& source :
         m_world.m_objectSimulation
             .leaseTransportGameplayTransactions()) {
        output.push_back({
            .kind = WorkKind::Transport,
            .transport = std::move(source),
        });
    }
}

void GameSessionWeaponEventDrain::sortWork(
    container::Vector<WorkItem>& pending) {
    std::stable_sort(
        pending.begin(), pending.end(),
        [this](const WorkItem& left, const WorkItem& right) {
            return gameplay::gameplayEnvelopeLess(
                workEnvelope(left), workEnvelope(right));
        });
}

void GameSessionWeaponEventDrain::pushPendingWork(
    container::Vector<WorkItem> pending) {
    for (WorkItem& item : pending) {
        if (workSequence(item) != 0) continue;
        // Legacy/direct callers without a stamped admission ordinal do not
        // get to create a zero-key tie whose outcome falls back to WorkKind.
        // Stamp them once at this owner-thread boundary. Migrated producers
        // never take this path.
        item.admissionOrdinal =
            m_world.m_objectSimulation
                .reserveGameplaySubmissionOrdinal();
    }
    sortWork(pending);
    container::Vector<gameplay::GameplayTransactionToken> admitted;
    admitted.reserve(pending.size());
    for (WorkItem& item : pending) {
        admitted.push_back(storeWork(std::move(item)));
    }
    for (auto iterator = admitted.rbegin(); iterator != admitted.rend();
         ++iterator) {
        m_work.push_back(*iterator);
    }
}

gameplay::GameplayTransactionToken
GameSessionWeaponEventDrain::storeWork(WorkItem item) {
    gameplay::GameplayEnvelope envelope = workEnvelope(item);
    uint64_t& next = m_content
                         .m_nextGameplayStorageOrdinal;
    envelope.submissionOrdinal = next++;
    if (next == 0) ++next;
    return m_storage.store(envelope, std::move(item));
}

void GameSessionWeaponEventDrain::pushWork(WorkItem item) {
    m_work.push_back(storeWork(std::move(item)));
}

void GameSessionWeaponEventDrain::closeCurrentReaction() {
    // A handler may submit Body damage (NeutronBlast/Dam) together with
    // Weapon/OCL/Spawn work. Admit that complete suffix into the common
    // journal, then close it above the handler's existing sibling stack.
    const size_t floor = m_work.size();
    container::Vector<WorkItem> nested;
    collectPendingWork(nested);
    pushPendingWork(std::move(nested));
    drainToSize(floor);
}

void GameSessionWeaponEventDrain::discardPendingWork() {
    m_reservedStructuralTransactions = 0;
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    simulation.discardQueuedDamageTransactions();
    simulation.discardConfirmedGameplayEvents();
    m_world.m_pendingProductionSpawns.clear();
    m_world.m_pendingProductionUpgrades.clear();
}

void GameSessionWeaponEventDrain::finalizeOcl(
    const ObjectCreationListInvocation& invocation,
    const container::SharedPtr<OclWorkState>& state) {
    if (!state || state->completionApplied) return;
    state->completionApplied = true;
    const ObjectCreationListCompletion& completion = invocation.completion;
    const auto resolveTransferredDamage = [&](ObjectDamageRequest request) {
        m_world.m_objectSimulation.queueDamage(std::move(request));
        closeCurrentReaction();
    };
    if (completion.kind ==
            ObjectCreationListCompletionKind::CreateObjectDie &&
        state->firstCreatedObject &&
        completion.transferPreviousHealth) {
        if (completion.subdualDamage > ObjectHealthComponent::Scalar{}) {
            resolveTransferredDamage({
                .target = state->firstCreatedObject,
                .source = completion.damageSource,
                .amount = completion.subdualDamage,
                .damageType = game::DamageType::SUBDUAL_UNRESISTABLE,
                .deathType = game::DeathType::NORMAL,
                .confirmedTick = invocation.confirmedTick,
            });
        }
        const ObjectHealthComponent::Scalar healthDamage =
            completion.maximumHealth - completion.previousHealth;
        if (healthDamage > ObjectHealthComponent::Scalar{}) {
            resolveTransferredDamage({
                .target = state->firstCreatedObject,
                .source = completion.damageSource,
                .amount = healthDamage,
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::NORMAL,
                .confirmedTick = invocation.confirmedTick,
            });
        }
        m_targetRemap.remapAttackTargets(invocation.source,
                                 state->firstCreatedObject);
    }
    if (invocation.resumeSourceUpgradeMux) {
        const std::optional<ecs::entity> source =
            m_world.m_objects.entityFromId(invocation.source);
        const OwnerComponent* owner = source
            ? ecs::try_get<OwnerComponent>(m_world.m_registry, *source)
            : nullptr;
        const PlayerState* player = owner
            ? m_content.m_players.get(owner->player) : nullptr;
        if (source && owner && player) {
            m_world.m_objectSimulation.activateInitialObjectUpgrades(
                m_world.m_registry, m_world.m_objects, invocation.source,
                player->upgrades.completed,
                invocation.confirmedTick,
                {.players = &m_content.m_players,
                 .scienceCatalog =
                     m_content.m_contentSnapshot.scienceCatalog(),
                 .content = &m_content.m_contentSnapshot,
                 .random = &m_content.m_simulationRandom,
                 .terrain = &m_content.m_terrain,
                 .spatialIndex = &m_world.m_spatialIndex,
                 .effects = &m_world.m_objectSimulation});
            container::Vector<WorkItem> nested;
            collectPendingWork(nested);
            pushPendingWork(std::move(nested));
        }
    }
}

bool GameSessionWeaponEventDrain::handleWeapon(WorkItem item) {
    return processWeapon(std::move(item));
}

bool GameSessionWeaponEventDrain::handleWeaponImpact(WorkItem item) {
    return processWeaponImpact(std::move(item));
}

bool GameSessionWeaponEventDrain::handleDamage(WorkItem item) {
    return processDamage(std::move(item));
}

bool GameSessionWeaponEventDrain::handleOcl(WorkItem item) {
    return processOcl(std::move(item));
}

bool GameSessionWeaponEventDrain::handleCrate(WorkItem item) {
    processCrate(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleReplacement(WorkItem item) {
    return processReplacement(item);
}

bool GameSessionWeaponEventDrain::handleUpgradeFx(WorkItem item) {
    processUpgradeFx(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleStructureFx(WorkItem item) {
    processStructureFx(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleTransitionOcl(WorkItem item) {
    return processTransitionOcl(item);
}

bool GameSessionWeaponEventDrain::handleInstantDeath(WorkItem item) {
    return processInstantDeath(item);
}

bool GameSessionWeaponEventDrain::handleSlowDeath(WorkItem item) {
    return processSlowDeath(item);
}

bool GameSessionWeaponEventDrain::handleSlowDeathRubble(WorkItem item) {
    processSlowDeathRubble(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleTopplePathfind(WorkItem item) {
    processTopplePathfind(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleToppleStump(WorkItem item) {
    processToppleStump(item);
    return true;
}

bool GameSessionWeaponEventDrain::handlePhysicsCrash(WorkItem item) {
    processPhysicsCrash(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleDestroyObject(WorkItem item) {
    processDestroyObject(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleMineSpawn(WorkItem item) {
    processMineSpawn(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleParticleUplinkRemnant(WorkItem item) {
    processParticleUplinkRemnant(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleWaveBridgeImpact(WorkItem item) {
    processWaveBridgeImpact(item);
    return true;
}

bool GameSessionWeaponEventDrain::handleDeathWalk(WorkItem item) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    const bool hasAuthoredBehavior =
        item.deathWalk.containmentDeathFinalize.has_value() ||
        (item.deathWalk.plan && item.deathWalk.hasReactionComponent &&
         (item.deathWalk.phase == ObjectDeathWalkPhase::Preamble ||
          item.deathWalk.phase == ObjectDeathWalkPhase::Behaviors) &&
         item.deathWalk.nextBehaviorIndex <
             item.deathWalk.plan->onDieBehaviors.size());
    const ObjectDeathWalkAdvance advance = simulation.advanceDeathWalk(
        m_world.m_registry,
        m_world.m_objects,
        item.deathWalk,
        {.players = &m_content.m_players,
         .scienceCatalog = m_content
                               .m_contentSnapshot.scienceCatalog(),
         .content = &m_content.m_contentSnapshot,
         .random = &m_content.m_simulationRandom,
         .terrain = &m_content.m_terrain,
         .navigation = &m_content.m_navigation,
         .effects = &simulation});
    if (advance == ObjectDeathWalkAdvance::InvalidState) {
        return false;
    }
    if (advance == ObjectDeathWalkAdvance::ReadyForPostamble) {
        const ObjectId deathObject = item.deathWalk.damage.target;
        if (!simulation.completeDeathWalk(std::move(item.deathWalk))) {
            return false;
        }
        // ZH Object::onDie notifies the current Team in the fixed object
        // suffix, after authored Die handlers and before reconstructing-hole
        // target transfer.  Produce the script hook at that same boundary;
        // the script runtime remains the sole consumer of the queued hook.
        if (const std::optional<ObjectTeamId> team =
                m_world.m_objectTeams.teamOf(
                    deathObject)) {
            m_objectEvents
                .m_teamUnitDestroyedHookEvents.push_back({.team = *team});
        }
        // The fixed death suffix can emit capability transactions of its own
        // (currently reconstructing-building target remap). Close them before
        // returning to the caller/sibling that caused this death.
        container::Vector<WorkItem> children;
        collectPendingWork(children);
        pushPendingWork(std::move(children));
        return true;
    }
    if (!hasAuthoredBehavior) return false;

    // LIFO call-stack rule: preserve the already-advanced continuation below
    // every child emitted by this one authored handler. The next Behavior can
    // therefore observe the complete state left by Damage/Weapon/OCL/Spawn.
    pushWork({
        .kind = WorkKind::DeathWalk,
        .deathWalk = std::move(item.deathWalk),
    });
    container::Vector<WorkItem> children;
    collectPendingWork(children);
    pushPendingWork(std::move(children));
    return true;
}

bool GameSessionWeaponEventDrain::handleDeleteWalk(WorkItem item) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    const bool hasAuthoredEntry = item.deleteWalk.plan &&
        item.deleteWalk.phase == ObjectDeleteWalkPhase::Behaviors &&
        item.deleteWalk.nextEntryIndex <
            item.deleteWalk.plan->entries.size();
    const ObjectUpgradeExecutionContext context{
        .players = &m_content.m_players,
        .scienceCatalog = m_content
                              .m_contentSnapshot.scienceCatalog(),
        .content = &m_content.m_contentSnapshot,
        .random = &m_content.m_simulationRandom,
        .terrain = &m_content.m_terrain,
        .spatialIndex = &m_world.m_spatialIndex,
        .navigation = &m_content.m_navigation,
        .effects = &simulation,
    };
    const ObjectDeleteWalkAdvance advance = simulation.advanceDeleteWalk(
        m_world.m_registry,
        m_world.m_objects,
        item.deleteWalk, context);
    if (advance == ObjectDeleteWalkAdvance::InvalidState) return false;
    if (advance == ObjectDeleteWalkAdvance::ReadyForPostamble) {
        if (!simulation.completeDeleteWalk(
                m_world.m_registry,
                m_world.m_objects,
                std::move(item.deleteWalk), context)) {
            return false;
        }
        m_deletePostamble.consume();
        if (m_frame.result().faulted()) return false;
        container::Vector<WorkItem> children;
        collectPendingWork(children);
        pushPendingWork(std::move(children));
        return true;
    }
    if (!hasAuthoredEntry) return false;

    // Preserve the already-advanced continuation below the exact module's
    // child Damage/Destroy suffix. A child object therefore completes its
    // own onDelete walk before the parent advances to the next authored slot.
    pushWork({
        .kind = WorkKind::DeleteWalk,
        .deleteWalk = std::move(item.deleteWalk),
    });
    container::Vector<WorkItem> children;
    collectPendingWork(children);
    pushPendingWork(std::move(children));
    return true;
}

bool GameSessionWeaponEventDrain::handleBodyResume(WorkItem item) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    if (!simulation.resumeDamageTransaction(
            m_world.m_registry,
            m_world.m_objects,
            item.bodyResume,
            {.players = &m_content.m_players,
             .scienceCatalog = m_content
                                   .m_contentSnapshot.scienceCatalog(),
             .content =
                 &m_content.m_contentSnapshot,
             .random =
                 &m_content.m_simulationRandom,
             .terrain = &m_content.m_terrain,
             .effects = &simulation})) {
        return false;
    }
    container::Vector<WorkItem> children;
    collectPendingWork(children);
    pushPendingWork(std::move(children));
    return true;
}

bool GameSessionWeaponEventDrain::handleOwnershipChange(WorkItem item) {
    const ObjectOwnershipChangeRequest& request = item.ownershipChange;
    if (!request.object || !request.owner ||
        !request.useOwnerDefaultTeam) {
        return false;
    }
    const std::optional<ObjectTeamId> targetTeam =
        m_world.m_objectTeams.defaultTeam(
            request.owner);
    if (!targetTeam) return false;
    const std::optional<ecs::entity> entity =
        m_world.m_objects
            .entityFromIdIncludingPending(request.object);
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(
              m_world.m_registry, *entity)
        : nullptr;
    const PrimaryTeamComponent* team = entity
        ? ecs::try_get<PrimaryTeamComponent>(
              m_world.m_registry, *entity)
        : nullptr;
    if (owner && team && owner->player == request.owner &&
        team->team == *targetTeam) {
        return true;
    }
    return m_ownership.transferObjectToTeam(
        request.object, *targetTeam, request.confirmedTick);
}

bool GameSessionWeaponEventDrain::handleDefection(WorkItem item) {
    static_cast<void>(m_ownership.applyDefection(item.defection));
    return true;
}

bool GameSessionWeaponEventDrain::handlePilotVehicleTakeover(
    WorkItem item) {
    static_cast<void>(m_ownership.applyPilotVehicleTakeover(
        item.pilotVehicleTakeover));
    return true;
}

bool GameSessionWeaponEventDrain::handleRailedTransportDockAttach(
    WorkItem item) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    const bool accepted =
        simulation.commitRailedTransportDockAttachCompletion(
            m_world.m_registry,
            m_world.m_objects,
            item.railedTransportDockAttach);
    // The structural edge and its ContainmentEvent side effects are one ZH
    // dock occurrence. Finish ownership/Academy/script reactions before the
    // next dock, carriage spawn or railroad collision sibling.
    closeCurrentReaction();
    simulation.acknowledgeRailedTransportDockAttachCompletion(
        m_world.m_registry,
        m_world.m_objects,
        item.railedTransportDockAttach, accepted);
    return true;
}

bool GameSessionWeaponEventDrain::handleRailroadDisembark(WorkItem item) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    if (!simulation.executeRailroadDisembark(
            m_world.m_registry,
            m_world.m_objects,
            item.railroadDisembark)) {
        return false;
    }
    closeCurrentReaction();
    return true;
}

bool GameSessionWeaponEventDrain::handleRailroadCarriageSpawn(
    WorkItem item) {
    const ObjectRailroadCarriageSpawnRequest& request =
        item.railroadCarriageSpawn;
    ObjectSpawnRequest spawn;
    spawn.templateName = request.templateName;
    spawn.owner = request.owner;
    spawn.primaryTeam = request.primaryTeam;
    spawn.transform = request.transform;
    spawn.origin = ObjectCreationOrigin::System;
    spawn.confirmedTick = request.confirmedTick;
    spawn.producer = request.locomotive;
    const GameSessionObjectSpawnResult carriage =
        m_lifecycle.spawnObject(std::move(spawn));
    closeCurrentReaction();
    const bool accepted = static_cast<bool>(carriage);
    if (m_world.m_objectSimulation
            .acknowledgeRailroadCarriageSpawn(
                m_world.m_registry,
                m_world.m_objects, request,
                carriage ? carriage.object : INVALID_OBJECT_ID,
                accepted)) {
        return true;
    }
    if (carriage) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            carriage.object, ObjectDestroyReason::System,
            request.confirmedTick));
        closeCurrentReaction();
    }
    static_cast<void>(m_publication.raiseSimulationFault({
        .domain = SimulationFaultDomain::Production,
        .code = SimulationFaultCode::AcknowledgementLost,
        .confirmedTick = request.confirmedTick,
        .subject = request.locomotive.value,
        .sequence = request.requestSequence,
    }));
    TD_LOG_ERROR(
        "[GameSession] Railroad carriage acknowledgement lost locomotive={} rule={} request={}",
        request.locomotive.value, request.railroadRuleIndex,
        request.requestSequence);
    return true;
}

bool GameSessionWeaponEventDrain::handleSpawnSlave(WorkItem item) {
    ObjectSpawnSlaveRequest& request = item.spawnSlave;
    GameSessionObjectSpawnResult spawned;
    if (request.reclaimedObject) {
        spawned.object = request.reclaimedObject;
        spawned.entity = m_world.m_objects
            .entityFromId(request.reclaimedObject);
    } else {
        ObjectSpawnRequest spawn;
        spawn.templateName = request.templateName;
        spawn.owner = request.owner;
        spawn.primaryTeam = request.primaryTeam;
        spawn.transform = ObjectFixedTransformComponent{
            .position = request.position,
            .yawRadians = request.yawRadians,
            .authoritative = true,
        };
        spawn.initialPathfindLayer = request.initialPathfindLayer;
        spawn.origin = ObjectCreationOrigin::System;
        spawn.confirmedTick = request.confirmedTick;
        spawn.producer = request.spawner;
        spawn.scoreAsBuilt = request.scoreAsBuilt;
        spawned = m_lifecycle.spawnObject(std::move(spawn));
        closeCurrentReaction();
    }
    bool accepted = static_cast<bool>(spawned);
    if (spawned && !request.reclaimedObject && request.containInSpawner) {
        accepted = m_world.m_objectSimulation
            .containObject(
                m_world.m_registry,
                m_world.m_objects,
                {.container = request.spawner,
                 .object = spawned.object,
                 .destroyWithContainer = false,
                 .followsContainerTransform = false});
        if (!accepted) {
            static_cast<void>(m_lifecycle.requestDestroyObject(
                spawned.object, ObjectDestroyReason::System,
                request.confirmedTick));
            closeCurrentReaction();
        }
    }
    const bool acknowledged = m_world
        .m_objectSimulation.acknowledgeSpawnSlave(
            m_world.m_registry,
            m_world.m_objects, request,
            spawned ? spawned.object : INVALID_OBJECT_ID, accepted);
    if (!acknowledged) {
        if (spawned && accepted && !request.reclaimedObject) {
            static_cast<void>(m_lifecycle.requestDestroyObject(
                spawned.object, ObjectDestroyReason::System,
                request.confirmedTick));
            closeCurrentReaction();
        }
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::Production,
            .code = SimulationFaultCode::AcknowledgementLost,
            .confirmedTick = request.confirmedTick,
            .subject = request.spawner.value,
            .sequence = static_cast<uint32_t>(request.requestId),
        }));
        TD_LOG_ERROR(
            "[GameSession] SpawnBehavior acknowledgement lost spawner={} rule={} request={}",
            request.spawner.value, request.ruleIndex, request.requestId);
        return true;
    }
    if (spawned && accepted && !request.containInSpawner &&
        request.holdAfterSpawn) {
        static_cast<void>(ObjectDisabledSystem::setUntil(
            m_world.m_registry, *spawned.entity,
            ObjectDisabledReason::Held, OBJECT_DISABLED_FOREVER_TICK,
            request.confirmedTick));
    }
    if (spawned && accepted && !request.containInSpawner) {
        production_exit::queueSpawnSlaveRoute(
            m_world.m_registry,
            *spawned.entity, request);
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleSpecialPowerSpawn(WorkItem item) {
    ObjectSpecialPowerSpawnRequest& request = item.specialPowerSpawn;
    if (request.replacedObject) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            request.replacedObject, ObjectDestroyReason::System,
            request.confirmedTick));
        closeCurrentReaction();
    }

    GameSessionObjectSpawnResult spawned;
    if (!request.objectTemplate.empty() &&
        m_content.m_contentSnapshot
            .findObjectArchetype(request.objectTemplate)) {
        ObjectSpawnRequest spawn;
        spawn.templateName = std::move(request.objectTemplate);
        spawn.owner = request.owner;
        spawn.primaryTeam = request.primaryTeam;
        spawn.transform = ObjectFixedTransformComponent{
            .position = request.position,
            .yawRadians = request.yawRadians,
            .authoritative = true,
        };
        spawn.origin = ObjectCreationOrigin::System;
        spawn.confirmedTick = request.confirmedTick;
        spawn.producer = request.source;
        spawned = m_lifecycle.spawnObject(std::move(spawn));
        // Spawn may publish onCreate Weapon/OCL/Damage children. They belong
        // to this spawn transaction and must finish before capability-specific
        // placement, containment and acknowledgement continue.
        closeCurrentReaction();
    }

    bool accepted = static_cast<bool>(spawned);
    const std::optional<ecs::entity> spawnedEntity = accepted
        ? m_world.m_objects.entityFromId(
              spawned.object)
        : std::nullopt;
    accepted = accepted && spawnedEntity.has_value();

    // RefCode SpecialPowerModule::createViewObject: the spawned view object
    // gets the SpecialPower's authored ViewObjectRange as its shroud-clearing
    // range and its DeletionUpdate timer is re-armed to ViewObjectDuration.
    // Both are applied here because GameSession owns object creation; the
    // simulation only published the value request.
    if (accepted && request.shroudClearingRange > math::q32_32{}) {
        setObjectVisionRangeOverride(
            m_world.m_registry, *spawnedEntity,
            effectiveObjectVisionRangeFixed(
                m_world.m_registry, *spawnedEntity),
            request.shroudClearingRange);
        if (request.viewObjectLifetimeFrames != 0 &&
            !m_world.m_objectSimulation.rescheduleLifetime(
                m_world.m_registry, m_world.m_objects,
                ObjectLifetimeRescheduleRequest{
                    .object = spawned.object,
                    .action = game::ObjectLifetimeAction::Destroy,
                    .minimumLifetimeFrames = request.viewObjectLifetimeFrames,
                    .maximumLifetimeFrames = request.viewObjectLifetimeFrames,
                    .confirmedTick = request.confirmedTick,
                })) {
            // The authored view object is expected to carry a DeletionUpdate
            // (stock SuperweaponPing does). Without one it would linger and
            // keep the target permanently revealed, so make that visible
            // rather than leaking a permanent scout.
            TD_LOG_WARN(
                "[GameSession] SpecialPower view object={} power={} has no DeletionUpdate timer to re-arm; it will not expire after {} frames",
                spawned.object.value, request.specialPower.value,
                request.viewObjectLifetimeFrames);
        }
    }

    if (accepted && request.projectPreferredHeight) {
        LogicFixedVec3 position = request.position;
        if (const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(
                    m_world.m_registry,
                    *spawnedEntity)) {
            position.z = math::q32_32::max(
                math::q32_32{}, locomotion->preferredHeightFixed);
        }
        writeAuthoritativeObjectPosition(
            m_world.m_registry,
            *spawnedEntity, position);
        if (ObjectPhysicsComponent* physics =
                ecs::try_get<ObjectPhysicsComponent>(
                    m_world.m_registry,
                    *spawnedEntity)) {
            physics->position = position;
            physics->lastPublishedPosition = position;
            physics->hasAuthoritativePosition = true;
        }
    }

    if (accepted && request.markAirborne) {
        if (ObjectAirborneComponent* airborne =
                ecs::try_get<ObjectAirborneComponent>(
                    m_world.m_registry,
                    *spawnedEntity)) {
            airborne->isAirborne = true;
        } else {
            ecs::emplace<ObjectAirborneComponent>(
                m_world.m_registry,
                *spawnedEntity, ObjectAirborneComponent{true});
        }
    }

    if (accepted && request.issueSpecialPowerOrder) {
        const SpecialPowerDefinition* definition =
            m_content.m_contentSnapshot
                .findSpecialPower(request.specialPower);
        if (!definition) {
            accepted = false;
        } else {
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(
                    m_world.m_registry,
                    *spawnedEntity);
            if (!queue) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    m_world.m_registry,
                    *spawnedEntity);
            }
            queue->orders.clear();
            queue->orders.push_back({
                .kind = ObjectOrderKind::SpecialPower,
                .source = ObjectOrderSource::System,
                .contextPlayer = request.owner,
                .issuedTick = request.confirmedTick,
                .sourceSequence = request.authoredOrder,
                .targetX = request.targetPosition.x,
                .targetY = request.targetPosition.y,
                .targetZ = request.targetPosition.z,
                .hasTargetPosition = true,
                .contentName = definition->name,
                .systemPurpose = ObjectOrderSystemPurpose::Generic,
            });
            ++queue->revision;
        }
    }

    if (accepted && request.attachToSourceContainment) {
        accepted = m_world.m_objectSimulation
            .containObject(
                m_world.m_registry,
                m_world.m_objects,
                {.container = request.source,
                 .object = spawned.object,
                 .containmentRuleIndex = request.containmentRuleIndex,
                 .confirmedEnteredTick = request.confirmedTick,
                 .destroyWithContainer = true,
                 .enclosing = true,
                 .followsContainerTransform = true});
    }

    if (accepted &&
        request.completion ==
            ObjectSpecialPowerSpawnCompletionKind::SpecialAbility &&
        request.attachStickyBomb) {
        accepted = m_world.m_objectSimulation
            .attachStickyBomb(
                m_world.m_registry,
                m_world.m_objects,
                m_content.m_terrain,
                {.bomb = spawned.object,
                 .target = request.target,
                 .bomber = request.source,
                 .specificPosition =
                     !request.target && request.hasEffectPosition
                         ? std::optional<LogicFixedVec3>{
                               request.effectPosition}
                         : std::nullopt,
                 .confirmedTick = request.confirmedTick});
    }

    bool acknowledged = true;
    if (request.completion !=
        ObjectSpecialPowerSpawnCompletionKind::None) {
        acknowledged = m_world
            .m_objectSimulation.acknowledgeSpecialPowerSpawn(
                m_world.m_registry,
                m_world.m_objects, request,
                accepted ? spawned.object : INVALID_OBJECT_ID, accepted);
    }
    closeCurrentReaction();
    if ((!accepted || !acknowledged) && spawned) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            spawned.object, ObjectDestroyReason::System,
            request.confirmedTick));
        closeCurrentReaction();
    }
    if (!acknowledged) {
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::Production,
            .code = SimulationFaultCode::AcknowledgementLost,
            .confirmedTick = request.confirmedTick,
            .subject = request.source.value,
            .sequence = static_cast<uint32_t>(request.emissionSequence),
        }));
    }
    return true;
}

bool GameSessionWeaponEventDrain::handleBridgeState(WorkItem item) {
    const ObjectBridgeStateEvent& event = item.bridgeState;
    static_cast<void>(m_presentation
        .m_scriptGameplayEvents.recordBridgeTransition({
            .bridge = event.object,
            .active = event.active,
            .confirmedTick = event.confirmedTick,
        }));

    container::Vector<navigation::NavigationCellId>& footprint =
        m_content.m_navigationFootprintScratch;
    if (m_content.m_navigation.isInitialized() &&
        footprint.size() < m_content
                               .m_navigation.grid().cellCount()) {
        footprint.resize(m_content
                             .m_navigation.grid().cellCount());
    }
    size_t footprintCount = 0;
    navigation::NavigationLayerId bridgeNavigationLayer;
    const std::optional<ecs::entity> entity =
        m_world.m_objects
            .entityFromIdIncludingPending(event.object);
    if (entity) {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(
                m_world.m_registry, *entity);
        const MapObjectProvenanceComponent* provenance =
            ecs::try_get<MapObjectProvenanceComponent>(
                m_world.m_registry, *entity);
        if (provenance && provenance->sourceRecordIndex != UINT64_MAX) {
            for (const game::terrain::TerrainElevatedPathfindSurface& surface :
                 m_content.m_terrain
                     .elevatedPathfindSurfaces()) {
                if (surface.sourceRecordIndex !=
                    provenance->sourceRecordIndex) {
                    continue;
                }
                if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(
                        surface.layer, bridgeNavigationLayer)) {
                    bridgeNavigationLayer = {};
                }
                break;
            }
            if (event.active) {
                // Repair reopens the preserved layer. Permanent WaveGuide
                // deletion remains authoritative in TerrainLogic.
                static_cast<void>(m_content
                    .m_terrain.setBridgeActiveBySourceRecordIndex(
                        provenance->sourceRecordIndex, true));
            } else if (event.deathOccurrence ||
                       (health && health->effectivelyDead)) {
                static_cast<void>(m_bridges.collapseTerrainSurface(
                    provenance->sourceRecordIndex, event.confirmedTick,
                    false));
                closeCurrentReaction();
            }
        }

        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(
                m_world.m_registry, *entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(
                m_world.m_registry, *entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(
                m_world.m_registry, *entity);
        if ((!mapStatus || !mapStatus->offMap) && transform && geometry &&
            m_content.m_navigation
                .isInitialized()) {
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry,
                *entity, *transform);
            const navigation::NavigationFootprintRasterResult raster =
                navigation::NavigationFootprintRasterizer::circle(
                    m_content.m_navigation
                        .grid(),
                    {position.x.raw(), position.y.raw(), 0},
                    math::q32_32::max(
                        math::q32_32{},
                        geometry->boundingCircleRadiusFixed).raw(),
                    footprint);
            if (raster.status ==
                navigation::NavigationFootprintRasterStatus::Success) {
                footprintCount = raster.writtenCount;
            }
        }
    }

    const navigation::NavigationDynamicOverlayResult result =
        m_navigation.submitBridgeState(
            event.object.value, bridgeNavigationLayer, event.active,
            container::Span<const navigation::NavigationCellId>{
                footprint.data(), footprintCount},
            event.confirmedTick);
    if (result == navigation::NavigationDynamicOverlayResult::Success ||
        result == navigation::NavigationDynamicOverlayResult::NotInitialized) {
        return true;
    }

    SimulationFaultCode faultCode = SimulationFaultCode::AtomicCommitFailed;
    switch (result) {
    case navigation::NavigationDynamicOverlayResult::InvalidCapacity:
    case navigation::NavigationDynamicOverlayResult::AllocationOverflow:
    case navigation::NavigationDynamicOverlayResult::EventCapacityExceeded:
    case navigation::NavigationDynamicOverlayResult::EntityCapacityExceeded:
    case navigation::NavigationDynamicOverlayResult::BridgeCapacityExceeded:
    case navigation::NavigationDynamicOverlayResult::FootprintCapacityExceeded:
    case navigation::NavigationDynamicOverlayResult::RefCountOverflow:
        faultCode = SimulationFaultCode::CapacityExceeded;
        break;
    case navigation::NavigationDynamicOverlayResult::InvalidEvent:
    case navigation::NavigationDynamicOverlayResult::InvalidCell:
    case navigation::NavigationDynamicOverlayResult::DuplicateCell:
    case navigation::NavigationDynamicOverlayResult::DuplicateEventKey:
    case navigation::NavigationDynamicOverlayResult::TickAlreadySealed:
        faultCode = SimulationFaultCode::InvalidEvent;
        break;
    default:
        break;
    }
    static_cast<void>(m_publication.raiseSimulationFault({
        .domain = SimulationFaultDomain::Navigation,
        .code = faultCode,
        .confirmedTick = event.confirmedTick,
        .subject = event.object.value,
    }));
    TD_LOG_WARN(
        "[GameSession] bridge navigation state for object {} at tick {} was rejected with status {}",
        event.object.value, event.confirmedTick,
        static_cast<uint32_t>(result));
    return true;
}

bool GameSessionWeaponEventDrain::handleSpecialAbilityEffect(WorkItem item) {
    ObjectSimulation& simulation =
        m_world.m_objectSimulation;
    if (!simulation.executeSpecialAbilityEffect(
            m_world.m_registry,
            m_world.m_objects,
            std::move(item.specialAbilityEffect),
            {.players = &m_content.m_players,
             .scienceCatalog = m_content
                                   .m_contentSnapshot.scienceCatalog(),
             .content =
                 &m_content.m_contentSnapshot,
             .random =
                 &m_content.m_simulationRandom,
             .terrain = &m_content.m_terrain,
             .spatialIndex =
                 &m_world.m_spatialIndex,
             .navigation =
                 &m_content.m_navigation,
             .mapVisibilityAuthority =
                 &m_world.m_mapVisibility,
             .effects = &simulation})) {
        return false;
    }
    // Every authored SpecialAbility occurrence owns the damage/spawn/OCL
    // suffix it emitted. Complete that suffix before the next ability sibling
    // is allowed to observe the world, matching the synchronous ZH callback.
    closeCurrentReaction();
    return true;
}

bool GameSessionWeaponEventDrain::handleSpecialPowerCompletion(
    WorkItem item) {
    ObjectSpecialPowerCompletionEvent& event =
        item.specialPowerCompletion;
    static_cast<void>(m_presentation
        .m_scriptGameplayEvents.recordSpecialPower({
            .phase = script::ScriptSpecialPowerEventPhase::Completed,
            .player = event.player,
            .source = event.creator,
            .specialPower = event.specialPowerTemplate,
            .confirmedTick = event.confirmedTick,
        }));
    return true;
}

bool GameSessionWeaponEventDrain::processOne(WorkItem item) {
    using Handler = bool (GameSessionWeaponEventDrain::*)(WorkItem);
    static constexpr auto handlers = std::to_array<Handler>({
        &GameSessionWeaponEventDrain::handleWeapon,
        &GameSessionWeaponEventDrain::handleWeaponImpact,
        &GameSessionWeaponEventDrain::handleDamage,
        &GameSessionWeaponEventDrain::handleOcl,
        &GameSessionWeaponEventDrain::handleCrate,
        &GameSessionWeaponEventDrain::handleReplacement,
        &GameSessionWeaponEventDrain::handleUpgradeFx,
        &GameSessionWeaponEventDrain::handleStructureFx,
        &GameSessionWeaponEventDrain::handleTransitionOcl,
        &GameSessionWeaponEventDrain::handleInstantDeath,
        &GameSessionWeaponEventDrain::handleSlowDeath,
        &GameSessionWeaponEventDrain::handleSlowDeathRubble,
        &GameSessionWeaponEventDrain::handleTopplePathfind,
        &GameSessionWeaponEventDrain::handleToppleStump,
        &GameSessionWeaponEventDrain::handlePhysicsCrash,
        &GameSessionWeaponEventDrain::handleAIMovementObstructionBatch,
        &GameSessionWeaponEventDrain::handleDestroyObject,
        &GameSessionWeaponEventDrain::handleMineSpawn,
        &GameSessionWeaponEventDrain::handleParticleUplinkRemnant,
        &GameSessionWeaponEventDrain::handleWaveBridgeImpact,
        &GameSessionWeaponEventDrain::handleCheckpointNavigation,
        &GameSessionWeaponEventDrain::handleTensileNavigation,
        &GameSessionWeaponEventDrain::handleDynamicGeometry,
        &GameSessionWeaponEventDrain::handleTransport,
        &GameSessionWeaponEventDrain::handleDeathWalk,
        &GameSessionWeaponEventDrain::handleDeleteWalk,
        &GameSessionWeaponEventDrain::handleBodyResume,
        &GameSessionWeaponEventDrain::handleOwnershipChange,
        &GameSessionWeaponEventDrain::handleDefection,
        &GameSessionWeaponEventDrain::handlePilotVehicleTakeover,
        &GameSessionWeaponEventDrain::handleRailedTransportDockAttach,
        &GameSessionWeaponEventDrain::handleRailroadDisembark,
        &GameSessionWeaponEventDrain::handleRailroadCarriageSpawn,
        &GameSessionWeaponEventDrain::handleSpawnSlave,
        &GameSessionWeaponEventDrain::handleSpecialPowerSpawn,
        &GameSessionWeaponEventDrain::handleBridgeState,
        &GameSessionWeaponEventDrain::handleConstructionCompletion,
        &GameSessionWeaponEventDrain::handleBridgeRepairScaffoldBatch,
        &GameSessionWeaponEventDrain::handleRebuildTargetRemap,
        &GameSessionWeaponEventDrain::handleRebuildHoleExpose,
        &GameSessionWeaponEventDrain::handleRebuildWorkerSpawn,
        &GameSessionWeaponEventDrain::handleRebuildCompletion,
        &GameSessionWeaponEventDrain::handleContainmentEvent,
        &GameSessionWeaponEventDrain::handleVehicleNeutralization,
        &GameSessionWeaponEventDrain::handleCratePickupBatch,
        &GameSessionWeaponEventDrain::handleCountermeasureFlareSpawn,
        &GameSessionWeaponEventDrain::handleProductionSpawn,
        &GameSessionWeaponEventDrain::handleProductionUpgrade,
        &GameSessionWeaponEventDrain::handleSpecialAbilityEffect,
        &GameSessionWeaponEventDrain::handleSpecialPowerCompletion,
    });
    static_assert(handlers.size() ==
        static_cast<size_t>(WorkKind::Count));
    const size_t index = static_cast<size_t>(item.kind);
    TD_ASSERT(index < handlers.size());
    return (this->*handlers[index])(std::move(item));
}

void GameSessionWeaponEventDrain::drainToSize(size_t floor) {
    while (m_work.size() > floor) {
        const gameplay::GameplayTransactionToken token = m_work.back();
        m_work.pop_back();
        const bool structural = token.kind == WorkKind::WeaponImpact ||
            token.kind == WorkKind::DeathWalk ||
            token.kind == WorkKind::DeleteWalk ||
            token.kind == WorkKind::BodyResume;
        if (structural) {
            TD_ASSERT(m_reservedStructuralTransactions != 0);
            if (m_reservedStructuralTransactions == 0) {
                discardPendingWork();
                m_work.clear();
                return;
            }
            --m_reservedStructuralTransactions;
        }
        if (++m_processedTransactions + m_reservedStructuralTransactions >
            kMaximumGameplayTransactions) {
            TD_LOG_ERROR(
                "[GameSession] Gameplay transaction chain exceeded {} entries at tick {} ordinal {}; aborting the complete malformed causal stack",
                kMaximumGameplayTransactions, token.envelope.confirmedTick,
                token.envelope.submissionOrdinal);
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::ObjectSimulation,
                .code = SimulationFaultCode::CapacityExceeded,
                .confirmedTick = token.envelope.confirmedTick,
                .subject = token.envelope.producer.value,
                .sequence = static_cast<uint32_t>(
                    token.envelope.submissionOrdinal),
            }));
            discardPendingWork();
            m_work.clear();
            return;
        }
        WorkItem item = m_storage.take(token);
        if (!processOne(std::move(item))) {
            static_cast<void>(m_publication.raiseSimulationFault({
                .domain = SimulationFaultDomain::ObjectSimulation,
                .code = SimulationFaultCode::InvalidEvent,
                .confirmedTick = token.envelope.confirmedTick,
                .subject = token.envelope.producer.value,
                .sequence = static_cast<uint32_t>(
                    token.envelope.submissionOrdinal),
            }));
            discardPendingWork();
            // A malformed transaction tail aborts the complete confirmed
            // causal stack, including outer siblings; resuming at a caller's
            // floor would expose a partially committed authored walk.
            m_work.clear();
            return;
        }
    }
}

void GameSessionWeaponEventDrain::run() {
    // RefCode forceFireWeapon is re-entrant: one reaction hit completes its
    // nested damage/death chain before the caller advances to the next death
    // module or radius target. The explicit LIFO stack preserves that
    // depth-first causal order across the value-only session boundary.
    container::Vector<WorkItem> initial;
    collectPendingWork(initial);
    pushPendingWork(std::move(initial));

    drainToSize(0);
}

} // namespace engine::detail
