#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"

#include "debug/debug.h"

#include <limits>
#include <utility>

namespace engine::detail {
namespace {

template <typename Payload>
[[nodiscard]] uint32_t storePayload(
    container::Vector<Payload>& pool, Payload payload) {
    TD_ASSERT(pool.size() < std::numeric_limits<uint32_t>::max());
    const uint32_t index = static_cast<uint32_t>(pool.size());
    pool.push_back(std::move(payload));
    return index;
}

template <typename Payload>
[[nodiscard]] Payload takePayload(
    container::Vector<Payload>& pool, uint32_t index) {
    TD_ASSERT(index < pool.size());
    return std::move(pool[index]);
}

} // namespace

gameplay::GameplayTransactionToken
GameSessionWeaponEventDrain::WorkStorage::store(
    gameplay::GameplayEnvelope envelope, WorkItem item) {
    gameplay::GameplayTransactionToken token{
        .envelope = envelope,
        .kind = item.kind,
    };
    switch (item.kind) {
    case WorkKind::Weapon:
    case WorkKind::WeaponImpact:
        token.payloadIndex = storePayload(weapons, std::move(item.weapon));
        break;
    case WorkKind::Damage:
        token.payloadIndex = storePayload(damages, std::move(item.damage));
        break;
    case WorkKind::ObjectCreationList:
        token.payloadIndex = storePayload(ocls, OclWorkPayload{
            .invocation = std::move(item.ocl),
            .nuggetIndex = item.oclNuggetIndex,
            .state = std::move(item.oclState),
        });
        break;
    case WorkKind::Crate:
        token.payloadIndex = storePayload(crates, std::move(item.crate));
        break;
    case WorkKind::Replacement:
        token.payloadIndex = storePayload(
            replacements, std::move(item.replacement));
        break;
    case WorkKind::UpgradeFx:
        token.payloadIndex = storePayload(upgradeFx, std::move(item.upgradeFx));
        break;
    case WorkKind::StructureFx:
        token.payloadIndex = storePayload(
            structureFx, std::move(item.structureFx));
        break;
    case WorkKind::TransitionOcl:
        token.payloadIndex = storePayload(
            transitionOcls, std::move(item.transitionOcl));
        break;
    case WorkKind::InstantDeath:
        token.payloadIndex = storePayload(
            instantDeaths, std::move(item.instantDeath));
        break;
    case WorkKind::SlowDeath:
        token.payloadIndex = storePayload(
            slowDeaths, std::move(item.slowDeath));
        break;
    case WorkKind::SlowDeathRubble:
        token.payloadIndex = storePayload(
            slowDeathRubbles, std::move(item.slowDeathRubble));
        break;
    case WorkKind::TopplePathfind:
        token.payloadIndex = storePayload(
            topplePathfind, std::move(item.topplePathfind));
        break;
    case WorkKind::ToppleStump:
        token.payloadIndex = storePayload(
            toppleStumps, std::move(item.toppleStump));
        break;
    case WorkKind::PhysicsCrash:
        token.payloadIndex = storePayload(
            physicsCrashes, std::move(item.physicsCrash));
        break;
    case WorkKind::AIMovementObstructionBatch:
        token.payloadIndex = storePayload(
            aiMovementObstructionBatches,
            std::move(item.aiMovementObstructionBatch));
        break;
    case WorkKind::DestroyObject:
        token.payloadIndex = storePayload(
            destroyObjects, std::move(item.destroyObject));
        break;
    case WorkKind::MineSpawn:
        token.payloadIndex = storePayload(
            mineSpawns, std::move(item.mineSpawn));
        break;
    case WorkKind::ParticleUplinkRemnant:
        token.payloadIndex = storePayload(
            particleUplinkRemnants,
            std::move(item.particleUplinkRemnant));
        break;
    case WorkKind::WaveBridgeImpact:
        token.payloadIndex = storePayload(
            waveBridgeImpacts, std::move(item.waveBridgeImpact));
        break;
    case WorkKind::CheckpointNavigation:
        token.payloadIndex = storePayload(
            checkpointNavigation, std::move(item.checkpointNavigation));
        break;
    case WorkKind::TensileNavigation:
        token.payloadIndex = storePayload(
            tensileNavigation, std::move(item.tensileNavigation));
        break;
    case WorkKind::DynamicGeometry:
        token.payloadIndex = storePayload(
            dynamicGeometry, std::move(item.dynamicGeometry));
        break;
    case WorkKind::Transport:
        token.payloadIndex = storePayload(
            transports, std::move(item.transport));
        break;
    case WorkKind::DeathWalk:
        token.payloadIndex = storePayload(
            deathWalks, std::move(item.deathWalk));
        break;
    case WorkKind::DeleteWalk:
        token.payloadIndex = storePayload(
            deleteWalks, std::move(item.deleteWalk));
        break;
    case WorkKind::BodyResume:
        token.payloadIndex = storePayload(
            bodyResumes, std::move(item.bodyResume));
        break;
    case WorkKind::OwnershipChange:
        token.payloadIndex = storePayload(
            ownershipChanges, std::move(item.ownershipChange));
        break;
    case WorkKind::Defection:
        token.payloadIndex = storePayload(
            defections, std::move(item.defection));
        break;
    case WorkKind::PilotVehicleTakeover:
        token.payloadIndex = storePayload(
            pilotVehicleTakeovers,
            std::move(item.pilotVehicleTakeover));
        break;
    case WorkKind::RailedTransportDockAttach:
        token.payloadIndex = storePayload(
            railedTransportDockAttaches,
            std::move(item.railedTransportDockAttach));
        break;
    case WorkKind::RailroadDisembark:
        token.payloadIndex = storePayload(
            railroadDisembarks, std::move(item.railroadDisembark));
        break;
    case WorkKind::RailroadCarriageSpawn:
        token.payloadIndex = storePayload(
            railroadCarriageSpawns,
            std::move(item.railroadCarriageSpawn));
        break;
    case WorkKind::SpawnSlave:
        token.payloadIndex = storePayload(
            spawnSlaves, std::move(item.spawnSlave));
        break;
    case WorkKind::SpecialPowerSpawn:
        token.payloadIndex = storePayload(
            specialPowerSpawns, std::move(item.specialPowerSpawn));
        break;
    case WorkKind::BridgeState:
        token.payloadIndex = storePayload(
            bridgeStates, std::move(item.bridgeState));
        break;
    case WorkKind::ConstructionCompletion:
        token.payloadIndex = storePayload(
            constructionCompletions,
            std::move(item.constructionCompletion));
        break;
    case WorkKind::BridgeRepairScaffoldBatch:
        token.payloadIndex = storePayload(
            bridgeRepairScaffoldBatches,
            std::move(item.bridgeRepairScaffoldBatch));
        break;
    case WorkKind::RebuildTargetRemap:
        token.payloadIndex = storePayload(
            rebuildTargetRemaps, std::move(item.rebuildTargetRemap));
        break;
    case WorkKind::RebuildHoleExpose:
        token.payloadIndex = storePayload(
            rebuildHoleExposes, std::move(item.rebuildHoleExpose));
        break;
    case WorkKind::RebuildWorkerSpawn:
        token.payloadIndex = storePayload(
            rebuildWorkerSpawns, std::move(item.rebuildWorkerSpawn));
        break;
    case WorkKind::RebuildCompletion:
        token.payloadIndex = storePayload(
            rebuildCompletions, std::move(item.rebuildCompletion));
        break;
    case WorkKind::ContainmentEvent:
        token.payloadIndex = storePayload(
            containmentEvents, std::move(item.containmentEvent));
        break;
    case WorkKind::VehicleNeutralization:
        token.payloadIndex = storePayload(
            vehicleNeutralizations, std::move(item.vehicleNeutralization));
        break;
    case WorkKind::CratePickupBatch:
        token.payloadIndex = storePayload(
            cratePickupBatches, std::move(item.cratePickupBatch));
        break;
    case WorkKind::CountermeasureFlareSpawn:
        token.payloadIndex = storePayload(
            countermeasureFlareSpawns,
            std::move(item.countermeasureFlareSpawn));
        break;
    case WorkKind::ProductionSpawn:
        token.payloadIndex = storePayload(
            productionSpawns, std::move(item.productionSpawn));
        break;
    case WorkKind::ProductionUpgrade:
        token.payloadIndex = storePayload(
            productionUpgrades, std::move(item.productionUpgrade));
        break;
    case WorkKind::SpecialAbilityEffect:
        token.payloadIndex = storePayload(
            specialAbilityEffects, std::move(item.specialAbilityEffect));
        break;
    case WorkKind::SpecialPowerCompletion:
        token.payloadIndex = storePayload(
            specialPowerCompletions,
            std::move(item.specialPowerCompletion));
        break;
    case WorkKind::Count:
        TD_ASSERT(false);
        break;
    }
    return token;
}

GameSessionWeaponEventDrain::WorkItem
GameSessionWeaponEventDrain::WorkStorage::take(
    const gameplay::GameplayTransactionToken& token) {
    WorkItem item;
    item.kind = token.kind;
    switch (token.kind) {
    case WorkKind::Weapon:
    case WorkKind::WeaponImpact:
        item.weapon = takePayload(weapons, token.payloadIndex);
        break;
    case WorkKind::Damage:
        item.damage = takePayload(damages, token.payloadIndex);
        break;
    case WorkKind::ObjectCreationList: {
        OclWorkPayload payload = takePayload(ocls, token.payloadIndex);
        item.ocl = std::move(payload.invocation);
        item.oclNuggetIndex = payload.nuggetIndex;
        item.oclState = std::move(payload.state);
        break;
    }
    case WorkKind::Crate:
        item.crate = takePayload(crates, token.payloadIndex);
        break;
    case WorkKind::Replacement:
        item.replacement = takePayload(replacements, token.payloadIndex);
        break;
    case WorkKind::UpgradeFx:
        item.upgradeFx = takePayload(upgradeFx, token.payloadIndex);
        break;
    case WorkKind::StructureFx:
        item.structureFx = takePayload(structureFx, token.payloadIndex);
        break;
    case WorkKind::TransitionOcl:
        item.transitionOcl = takePayload(transitionOcls, token.payloadIndex);
        break;
    case WorkKind::InstantDeath:
        item.instantDeath = takePayload(instantDeaths, token.payloadIndex);
        break;
    case WorkKind::SlowDeath:
        item.slowDeath = takePayload(slowDeaths, token.payloadIndex);
        break;
    case WorkKind::SlowDeathRubble:
        item.slowDeathRubble = takePayload(
            slowDeathRubbles, token.payloadIndex);
        break;
    case WorkKind::TopplePathfind:
        item.topplePathfind = takePayload(
            topplePathfind, token.payloadIndex);
        break;
    case WorkKind::ToppleStump:
        item.toppleStump = takePayload(toppleStumps, token.payloadIndex);
        break;
    case WorkKind::PhysicsCrash:
        item.physicsCrash = takePayload(physicsCrashes, token.payloadIndex);
        break;
    case WorkKind::AIMovementObstructionBatch:
        item.aiMovementObstructionBatch = takePayload(
            aiMovementObstructionBatches, token.payloadIndex);
        break;
    case WorkKind::DestroyObject:
        item.destroyObject = takePayload(destroyObjects, token.payloadIndex);
        break;
    case WorkKind::MineSpawn:
        item.mineSpawn = takePayload(mineSpawns, token.payloadIndex);
        break;
    case WorkKind::ParticleUplinkRemnant:
        item.particleUplinkRemnant = takePayload(
            particleUplinkRemnants, token.payloadIndex);
        break;
    case WorkKind::WaveBridgeImpact:
        item.waveBridgeImpact = takePayload(
            waveBridgeImpacts, token.payloadIndex);
        break;
    case WorkKind::CheckpointNavigation:
        item.checkpointNavigation = takePayload(
            checkpointNavigation, token.payloadIndex);
        break;
    case WorkKind::TensileNavigation:
        item.tensileNavigation = takePayload(
            tensileNavigation, token.payloadIndex);
        break;
    case WorkKind::DynamicGeometry:
        item.dynamicGeometry = takePayload(
            dynamicGeometry, token.payloadIndex);
        break;
    case WorkKind::Transport:
        item.transport = takePayload(transports, token.payloadIndex);
        break;
    case WorkKind::DeathWalk:
        item.deathWalk = takePayload(deathWalks, token.payloadIndex);
        break;
    case WorkKind::DeleteWalk:
        item.deleteWalk = takePayload(deleteWalks, token.payloadIndex);
        break;
    case WorkKind::BodyResume:
        item.bodyResume = takePayload(bodyResumes, token.payloadIndex);
        break;
    case WorkKind::OwnershipChange:
        item.ownershipChange = takePayload(
            ownershipChanges, token.payloadIndex);
        break;
    case WorkKind::Defection:
        item.defection = takePayload(defections, token.payloadIndex);
        break;
    case WorkKind::PilotVehicleTakeover:
        item.pilotVehicleTakeover = takePayload(
            pilotVehicleTakeovers, token.payloadIndex);
        break;
    case WorkKind::RailedTransportDockAttach:
        item.railedTransportDockAttach = takePayload(
            railedTransportDockAttaches, token.payloadIndex);
        break;
    case WorkKind::RailroadDisembark:
        item.railroadDisembark = takePayload(
            railroadDisembarks, token.payloadIndex);
        break;
    case WorkKind::RailroadCarriageSpawn:
        item.railroadCarriageSpawn = takePayload(
            railroadCarriageSpawns, token.payloadIndex);
        break;
    case WorkKind::SpawnSlave:
        item.spawnSlave = takePayload(spawnSlaves, token.payloadIndex);
        break;
    case WorkKind::SpecialPowerSpawn:
        item.specialPowerSpawn = takePayload(
            specialPowerSpawns, token.payloadIndex);
        break;
    case WorkKind::BridgeState:
        item.bridgeState = takePayload(bridgeStates, token.payloadIndex);
        break;
    case WorkKind::ConstructionCompletion:
        item.constructionCompletion = takePayload(
            constructionCompletions, token.payloadIndex);
        break;
    case WorkKind::BridgeRepairScaffoldBatch:
        item.bridgeRepairScaffoldBatch = takePayload(
            bridgeRepairScaffoldBatches, token.payloadIndex);
        break;
    case WorkKind::RebuildTargetRemap:
        item.rebuildTargetRemap = takePayload(
            rebuildTargetRemaps, token.payloadIndex);
        break;
    case WorkKind::RebuildHoleExpose:
        item.rebuildHoleExpose = takePayload(
            rebuildHoleExposes, token.payloadIndex);
        break;
    case WorkKind::RebuildWorkerSpawn:
        item.rebuildWorkerSpawn = takePayload(
            rebuildWorkerSpawns, token.payloadIndex);
        break;
    case WorkKind::RebuildCompletion:
        item.rebuildCompletion = takePayload(
            rebuildCompletions, token.payloadIndex);
        break;
    case WorkKind::ContainmentEvent:
        item.containmentEvent = takePayload(
            containmentEvents, token.payloadIndex);
        break;
    case WorkKind::VehicleNeutralization:
        item.vehicleNeutralization = takePayload(
            vehicleNeutralizations, token.payloadIndex);
        break;
    case WorkKind::CratePickupBatch:
        item.cratePickupBatch = takePayload(
            cratePickupBatches, token.payloadIndex);
        break;
    case WorkKind::CountermeasureFlareSpawn:
        item.countermeasureFlareSpawn = takePayload(
            countermeasureFlareSpawns, token.payloadIndex);
        break;
    case WorkKind::ProductionSpawn:
        item.productionSpawn = takePayload(
            productionSpawns, token.payloadIndex);
        break;
    case WorkKind::ProductionUpgrade:
        item.productionUpgrade = takePayload(
            productionUpgrades, token.payloadIndex);
        break;
    case WorkKind::SpecialAbilityEffect:
        item.specialAbilityEffect = takePayload(
            specialAbilityEffects, token.payloadIndex);
        break;
    case WorkKind::SpecialPowerCompletion:
        item.specialPowerCompletion = takePayload(
            specialPowerCompletions, token.payloadIndex);
        break;
    case WorkKind::Count:
        TD_ASSERT(false);
        break;
    }
    return item;
}

} // namespace engine::detail
