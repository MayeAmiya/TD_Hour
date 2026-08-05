#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace engine {

ObjectSimulationEventLease<ObjectDeleteWalkState>
ObjectSimulationLifecycleDomain::leaseObjectDeleteWalks() {
    return ObjectSimulationEventLease<ObjectDeleteWalkState>{
        object_simulation_detail::state(*this).m_deleteWalks};
}

container::Vector<ObjectDeleteWalkState>
ObjectSimulationLifecycleDomain::takeObjectDeleteWalks() {
    container::Vector<ObjectDeleteWalkState> result =
        std::move(object_simulation_detail::state(*this).m_deleteWalks);
    object_simulation_detail::state(*this).m_deleteWalks.clear();
    return result;
}

container::Vector<ObjectDeletePostambleEvent>
ObjectSimulationLifecycleDomain::takeObjectDeletePostambleEvents() {
    container::Vector<ObjectDeletePostambleEvent> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_deletePostambleEvents);
    return result;
}

container::Vector<ObjectDeleteDestroyRequest>
ObjectSimulationLifecycleDomain::takeObjectDeleteDestroyRequests() {
    container::Vector<ObjectDeleteDestroyRequest> result = std::move(
        object_simulation_detail::state(*this).m_deleteDestroyRequests);
    object_simulation_detail::state(*this).m_deleteDestroyRequests.clear();
    return result;
}

ObjectSimulationEventLease<ObjectDeleteDestroyRequest>
ObjectSimulationLifecycleDomain::leaseObjectDeleteDestroyRequests() {
    return ObjectSimulationEventLease<ObjectDeleteDestroyRequest>{
        object_simulation_detail::state(*this).m_deleteDestroyRequests};
}

container::Vector<ObjectHealthEvent> ObjectSimulationDamageDomain::takeHealthEvents() {
    container::Vector<ObjectHealthEvent> result = std::move(object_simulation_detail::state(*this).m_healthEvents);
    object_simulation_detail::state(*this).m_healthEvents.clear();
    // Ownership ordinals exist only while ObjectSimulation closes re-entrant
    // Body consumers. They are not part of frame events, diagnostics,
    // replay/network contracts or presentation state.
    for (ObjectHealthEvent& event : result) {
        event.bodyTransactionOrdinal = 0;
    }
    return result;
}

void ObjectSimulationDamageDomain::drainHealthEvents(
    container::Vector<ObjectHealthEvent>& out) {
    auto& source = object_simulation_detail::state(*this).m_healthEvents;
    out.clear();
    out.reserve(source.size());
    out.insert(out.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end()));
    source.clear();
    for (ObjectHealthEvent& event : out) {
        event.bodyTransactionOrdinal = 0;
    }
}

container::Vector<ObjectTransitionDamageFxEvent>
ObjectSimulationDamageDomain::takeTransitionDamageGameplayEvents() {
    auto leased = leaseTransitionDamageGameplayEvents();
    container::Vector<ObjectTransitionDamageFxEvent> output;
    output.reserve(leased.size());
    for (ObjectTransitionDamageFxEvent& event : leased) {
        output.push_back(std::move(event));
    }
    return output;
}

ObjectSimulationEventLease<ObjectTransitionDamageFxEvent>
ObjectSimulationDamageDomain::leaseTransitionDamageGameplayEvents() {
    auto& state = object_simulation_detail::state(*this);
    auto& source = state.m_transitionDamageFxEvents;
    auto& gameplay = state.m_transitionDamageGameplayScratch;
    gameplay.clear();
    gameplay.reserve(source.size());
    size_t presentationCount = 0;
    for (size_t index = 0; index < source.size(); ++index) {
        ObjectTransitionDamageFxEvent& event = source[index];
        if (event.kind ==
            ObjectTransitionDamageFxEventKind::ObjectCreationList) {
            gameplay.push_back(std::move(event));
        } else {
            if (presentationCount != index) {
                source[presentationCount] = std::move(event);
            }
            ++presentationCount;
        }
    }
    source.resize(presentationCount);
    return ObjectSimulationEventLease<ObjectTransitionDamageFxEvent>{gameplay};
}

container::Vector<ObjectTransitionDamageFxEvent>
ObjectSimulationDamageDomain::takeTransitionDamageFxEvents() {
    container::Vector<ObjectTransitionDamageFxEvent> result =
        std::move(object_simulation_detail::state(*this).m_transitionDamageFxEvents);
    object_simulation_detail::state(*this).m_transitionDamageFxEvents.clear();
    return result;
}

container::Vector<ObjectStructureEffectEvent>
ObjectSimulationLifecycleDomain::takeStructureEffectEvents() {
    container::Vector<ObjectStructureEffectEvent> result =
        std::move(object_simulation_detail::state(*this).m_structureEffectEvents);
    object_simulation_detail::state(*this).m_structureEffectEvents.clear();
    return result;
}

ObjectSimulationEventLease<ObjectStructureEffectEvent>
ObjectSimulationLifecycleDomain::leaseStructureEffectEvents() {
    return ObjectSimulationEventLease<ObjectStructureEffectEvent>{
        object_simulation_detail::state(*this).m_structureEffectEvents};
}

container::Vector<ObjectEmpParticleEvent>
ObjectSimulationAbilityDomain::takeObjectEmpParticleEvents() {
    container::Vector<ObjectEmpParticleEvent> result =
        std::move(object_simulation_detail::state(*this).m_empParticleEvents);
    object_simulation_detail::state(*this).m_empParticleEvents.clear();
    return result;
}

container::Vector<ObjectAutoHealParticleEvent>
ObjectSimulationAbilityDomain::takeObjectAutoHealParticleEvents() {
    container::Vector<ObjectAutoHealParticleEvent> result =
        std::move(object_simulation_detail::state(*this).m_autoHealParticleEvents);
    object_simulation_detail::state(*this).m_autoHealParticleEvents.clear();
    return result;
}

container::Vector<ObjectLeafletParticleEvent>
ObjectSimulationAbilityDomain::takeObjectLeafletParticleEvents() {
    container::Vector<ObjectLeafletParticleEvent> result =
        std::move(object_simulation_detail::state(*this).m_leafletParticleEvents);
    object_simulation_detail::state(*this).m_leafletParticleEvents.clear();
    return result;
}

container::Vector<ObjectStealthDetectorPulseEvent>
ObjectSimulationAbilityDomain::takeStealthDetectorPulseEvents() {
    container::Vector<ObjectStealthDetectorPulseEvent> result =
        std::move(object_simulation_detail::state(*this).m_stealthDetectorPulseEvents);
    object_simulation_detail::state(*this).m_stealthDetectorPulseEvents.clear();
    return result;
}

container::Vector<ObjectGrantStealthPulseEvent>
ObjectSimulationAbilityDomain::takeGrantStealthPulseEvents() {
    container::Vector<ObjectGrantStealthPulseEvent> result =
        std::move(object_simulation_detail::state(*this).m_grantStealthPulseEvents);
    object_simulation_detail::state(*this).m_grantStealthPulseEvents.clear();
    return result;
}

container::Vector<ObjectDynamicShroudDecalEvent>
ObjectSimulationAbilityDomain::takeDynamicShroudDecalEvents() {
    container::Vector<ObjectDynamicShroudDecalEvent> result =
        std::move(object_simulation_detail::state(*this).m_dynamicShroudDecalEvents);
    object_simulation_detail::state(*this).m_dynamicShroudDecalEvents.clear();
    return result;
}

container::Vector<ObjectRadiusDecalEvent>
ObjectSimulationAbilityDomain::takeRadiusDecalEvents() {
    container::Vector<ObjectRadiusDecalEvent> result =
        std::move(object_simulation_detail::state(*this).m_radiusDecalEvents);
    object_simulation_detail::state(*this).m_radiusDecalEvents.clear();
    return result;
}

container::Vector<ObjectAirfieldEvent>
ObjectSimulationAirOperationsDomain::takeAirfieldEvents() {
    container::Vector<ObjectAirfieldEvent> result = std::move(object_simulation_detail::state(*this).m_airfieldEvents);
    object_simulation_detail::state(*this).m_airfieldEvents.clear();
    return result;
}

container::Vector<ObjectAirfieldAutomaticProductionRequest>
ObjectSimulationAirOperationsDomain::
    takeAirfieldAutomaticProductionRequests() {
    container::Vector<ObjectAirfieldAutomaticProductionRequest> result =
        std::move(object_simulation_detail::state(*this).
                      m_airfieldAutomaticProductionRequests);
    object_simulation_detail::state(*this).
        m_airfieldAutomaticProductionRequests.clear();
    return result;
}

container::Vector<ObjectChinookRopePresentationEvent>
ObjectSimulationAirOperationsDomain::takeChinookRopePresentationEvents() {
    container::Vector<ObjectChinookRopePresentationEvent> result =
        std::move(object_simulation_detail::state(*this).m_chinookRopePresentationEvents);
    object_simulation_detail::state(*this).m_chinookRopePresentationEvents.clear();
    return result;
}

container::Vector<ObjectTechBuildingEvent>
ObjectSimulationAbilityDomain::takeTechBuildingEvents() {
    container::Vector<ObjectTechBuildingEvent> result =
        std::move(object_simulation_detail::state(*this).m_techBuildingEvents);
    object_simulation_detail::state(*this).m_techBuildingEvents.clear();
    return result;
}

container::Vector<ObjectBeaconClientEvent>
ObjectSimulationAbilityDomain::takeBeaconClientEvents() {
    container::Vector<ObjectBeaconClientEvent> result =
        std::move(object_simulation_detail::state(*this).m_beaconClientEvents);
    object_simulation_detail::state(*this).m_beaconClientEvents.clear();
    return result;
}

container::Vector<ObjectCheckpointNavigationEvent>
ObjectSimulationAbilityDomain::takeCheckpointNavigationEvents() {
    container::Vector<ObjectCheckpointNavigationEvent> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_checkpointNavigationEvents);
    return result;
}

ObjectSimulationEventLease<ObjectCheckpointNavigationEvent>
ObjectSimulationAbilityDomain::leaseCheckpointNavigationEvents() {
    return ObjectSimulationEventLease<ObjectCheckpointNavigationEvent>{
        object_simulation_detail::state(*this).m_checkpointNavigationEvents};
}

container::Vector<ObjectDynamicGeometryGameplayEvent>
ObjectSimulationAbilityDomain::takeDynamicGeometryGameplayEvents() {
    container::Vector<ObjectDynamicGeometryGameplayEvent> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_dynamicGeometryGameplayEvents);
    return result;
}

ObjectSimulationEventLease<ObjectDynamicGeometryGameplayEvent>
ObjectSimulationAbilityDomain::leaseDynamicGeometryGameplayEvents() {
    return ObjectSimulationEventLease<ObjectDynamicGeometryGameplayEvent>{
        object_simulation_detail::state(*this)
            .m_dynamicGeometryGameplayEvents};
}

container::Vector<ObjectDynamicGeometryPresentationEvent>
ObjectSimulationAbilityDomain::takeDynamicGeometryPresentationEvents() {
    container::Vector<ObjectDynamicGeometryPresentationEvent> result =
        std::move(object_simulation_detail::state(*this).m_dynamicGeometryPresentationEvents);
    object_simulation_detail::state(*this).m_dynamicGeometryPresentationEvents.clear();
    return result;
}

container::Vector<ObjectBattlePlanPresentationEvent>
ObjectSimulationAbilityDomain::takeBattlePlanPresentationEvents() {
    return object_simulation_detail::state(*this)
        .m_tactical.takeBattlePlanPresentationEvents();
}

container::Vector<ObjectTacticalPresentationEvent>
ObjectSimulationAbilityDomain::takeTacticalPresentationEvents() {
    return object_simulation_detail::state(*this)
        .m_tactical.takeTacticalPresentationEvents();
}

container::Vector<ObjectDisguisePresentationEvent>
ObjectSimulationAbilityDomain::takeDisguisePresentationEvents() {
    return object_simulation_detail::state(*this)
        .m_stealth.takeDisguisePresentationEvents();
}

container::Vector<ObjectDeployStyleManualFrameEvent>
ObjectSimulationAbilityDomain::takeDeployStyleManualFrameEvents() {
    return object_simulation_detail::state(*this)
        .m_tactical.takeDeployStyleManualFrameEvents();
}

container::Vector<ObjectToppleFxEvent>
ObjectSimulationAbilityDomain::takeToppleFxEvents() {
    return object_simulation_detail::state(*this)
        .m_tactical.takeToppleFxEvents();
}

container::Vector<ObjectToppleStumpSpawnRequest>
ObjectSimulationAbilityDomain::takeToppleStumpSpawnRequests() {
    auto leased = leaseToppleStumpSpawnRequests();
    container::Vector<ObjectToppleStumpSpawnRequest> output;
    output.reserve(leased.size());
    for (ObjectToppleStumpSpawnRequest& request : leased) {
        output.push_back(std::move(request));
    }
    return output;
}

ObjectSimulationEventLease<ObjectToppleStumpSpawnRequest>
ObjectSimulationAbilityDomain::leaseToppleStumpSpawnRequests() {
    auto& state = object_simulation_detail::state(*this);
    state.m_tactical.drainToppleStumpSpawnRequests(
        state.m_toppleStumpGameplayScratch);
    return ObjectSimulationEventLease<ObjectToppleStumpSpawnRequest>{
        state.m_toppleStumpGameplayScratch};
}

container::Vector<ObjectTopplePathfindRemovalRequest>
ObjectSimulationAbilityDomain::takeTopplePathfindRemovalRequests() {
    auto leased = leaseTopplePathfindRemovalRequests();
    container::Vector<ObjectTopplePathfindRemovalRequest> output;
    output.reserve(leased.size());
    for (ObjectTopplePathfindRemovalRequest& request : leased) {
        output.push_back(std::move(request));
    }
    return output;
}

ObjectSimulationEventLease<ObjectTopplePathfindRemovalRequest>
ObjectSimulationAbilityDomain::leaseTopplePathfindRemovalRequests() {
    auto& state = object_simulation_detail::state(*this);
    state.m_tactical.drainTopplePathfindRemovalRequests(
        state.m_topplePathfindGameplayScratch);
    return ObjectSimulationEventLease<ObjectTopplePathfindRemovalRequest>{
        state.m_topplePathfindGameplayScratch};
}

container::Vector<ObjectSpecialPowerExecutionEvent>
ObjectSimulationAbilityDomain::takeSpecialPowerExecutionEvents() {
    container::Vector<ObjectSpecialPowerExecutionEvent> result;
    result.swap(object_simulation_detail::state(*this).m_specialPowerExecutionEvents);
    return result;
}

container::Vector<ObjectSpecialAbilityEffectRequest>
ObjectSimulationAbilityDomain::takeSpecialAbilityEffectRequests() {
    container::Vector<ObjectSpecialAbilityEffectRequest> result = std::move(
        object_simulation_detail::state(*this)
            .m_specialAbilityEffectRequests);
    object_simulation_detail::state(*this)
        .m_specialAbilityEffectRequests.clear();
    return result;
}

ObjectSimulationEventLease<ObjectSpecialAbilityEffectRequest>
ObjectSimulationAbilityDomain::leaseSpecialAbilityEffectRequests() {
    return ObjectSimulationEventLease<ObjectSpecialAbilityEffectRequest>{
        object_simulation_detail::state(*this)
            .m_specialAbilityEffectRequests};
}

container::Vector<ObjectSpecialAbilityFacingRequest>
ObjectSimulationAbilityDomain::takeSpecialAbilityFacingRequests() {
    container::Vector<ObjectSpecialAbilityFacingRequest> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_specialAbilityFacingRequests);
    return result;
}

container::Vector<ObjectDefectionRequest>
ObjectSimulationAbilityDomain::takeObjectDefectionRequests() {
    container::Vector<ObjectDefectionRequest> result;
    result.swap(object_simulation_detail::state(*this).m_objectDefectionRequests);
    return result;
}

ObjectSimulationEventLease<ObjectDefectionRequest>
ObjectSimulationAbilityDomain::leaseObjectDefectionRequests() {
    return ObjectSimulationEventLease<ObjectDefectionRequest>{
        object_simulation_detail::state(*this).m_objectDefectionRequests};
}

container::Vector<ObjectPilotVehicleTakeoverRequest>
ObjectSimulationAbilityDomain::takePilotVehicleTakeoverRequests() {
    container::Vector<ObjectPilotVehicleTakeoverRequest> result;
    result.swap(object_simulation_detail::state(*this).m_pilotVehicleTakeoverRequests);
    return result;
}

ObjectSimulationEventLease<ObjectPilotVehicleTakeoverRequest>
ObjectSimulationAbilityDomain::leasePilotVehicleTakeoverRequests() {
    return ObjectSimulationEventLease<ObjectPilotVehicleTakeoverRequest>{
        object_simulation_detail::state(*this)
            .m_pilotVehicleTakeoverRequests};
}

container::Vector<ObjectSpecialPowerSpawnRequest>
ObjectSimulationAbilityDomain::takeSpecialPowerSpawnRequests() {
    container::Vector<ObjectSpecialPowerSpawnRequest> result;
    result.swap(object_simulation_detail::state(*this).m_specialPowerSpawnRequests);
    return result;
}

ObjectSimulationEventLease<ObjectSpecialPowerSpawnRequest>
ObjectSimulationAbilityDomain::leaseSpecialPowerSpawnRequests() {
    return ObjectSimulationEventLease<ObjectSpecialPowerSpawnRequest>{
        object_simulation_detail::state(*this).m_specialPowerSpawnRequests};
}

container::Vector<ObjectCountermeasureFlareSpawnCommand>
ObjectSimulationAbilityDomain::takeCountermeasureFlareSpawnCommands() {
    auto leased = leaseCountermeasureFlareSpawnCommands();
    container::Vector<ObjectCountermeasureFlareSpawnCommand> output;
    output.reserve(leased.size());
    for (ObjectCountermeasureFlareSpawnCommand& command : leased) {
        output.push_back(std::move(command));
    }
    return output;
}

ObjectSimulationEventLease<ObjectCountermeasureFlareSpawnCommand>
ObjectSimulationAbilityDomain::leaseCountermeasureFlareSpawnCommands() {
    auto& state = object_simulation_detail::state(*this);
    state.m_countermeasures.drainFlareSpawnCommands(
        state.m_countermeasureFlareGameplayScratch);
    ObjectSimulationEventLease<ObjectCountermeasureFlareSpawnCommand> result{
        state.m_countermeasureFlareGameplayScratch};
    ObjectSimulation& simulation = static_cast<ObjectSimulation&>(*this);
    for (ObjectCountermeasureFlareSpawnCommand& command : result) {
        if (command.submissionOrdinal == 0) {
            command.submissionOrdinal =
                simulation.reserveGameplaySubmissionOrdinal();
        }
    }
    return result;
}

container::Vector<ObjectCountermeasureEvent>
ObjectSimulationAbilityDomain::takeCountermeasureEvents() {
    return object_simulation_detail::state(*this).m_countermeasures.takeEvents();
}

container::Vector<ObjectMineSpawnCommand>
ObjectSimulationAbilityDomain::takeMineSpawnCommands() {
    container::Vector<ObjectMineSpawnCommand> result;
    result.swap(object_simulation_detail::state(*this).m_mineSpawnCommands);
    return result;
}

ObjectSimulationEventLease<ObjectMineSpawnCommand>
ObjectSimulationAbilityDomain::leaseMineSpawnCommands() {
    return ObjectSimulationEventLease<ObjectMineSpawnCommand>{
        object_simulation_detail::state(*this).m_mineSpawnCommands};
}

container::Vector<ObjectMinefieldFxEvent>
ObjectSimulationAbilityDomain::takeMinefieldFxEvents() {
    container::Vector<ObjectMinefieldFxEvent> result;
    result.swap(object_simulation_detail::state(*this).m_minefieldFxEvents);
    return result;
}

container::Vector<ObjectStickyBombPresentationEvent>
ObjectSimulationAbilityDomain::takeStickyBombPresentationEvents() {
    container::Vector<ObjectStickyBombPresentationEvent> result;
    result.swap(object_simulation_detail::state(*this).m_stickyBombPresentationEvents);
    return result;
}

container::Vector<ObjectNeutronMissilePresentationEvent>
ObjectSimulationAbilityDomain::takeNeutronMissilePresentationEvents() {
    container::Vector<ObjectNeutronMissilePresentationEvent> result;
    result.swap(object_simulation_detail::state(*this).m_neutronMissilePresentationEvents);
    return result;
}

container::Vector<ObjectWaveGuideEvent>
ObjectSimulationAbilityDomain::takeWaveGuideEvents() {
    container::Vector<ObjectWaveGuideEvent> result;
    result.swap(object_simulation_detail::state(*this).m_waveGuideEvents);
    return result;
}

container::Vector<ObjectWaveGuideBridgeImpact>
ObjectSimulationAbilityDomain::takeWaveGuideBridgeImpacts() {
    container::Vector<ObjectWaveGuideBridgeImpact> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_waveGuideBridgeImpacts);
    return result;
}

ObjectSimulationEventLease<ObjectWaveGuideBridgeImpact>
ObjectSimulationAbilityDomain::leaseWaveGuideBridgeImpacts() {
    return ObjectSimulationEventLease<ObjectWaveGuideBridgeImpact>{
        object_simulation_detail::state(*this).m_waveGuideBridgeImpacts};
}

container::Vector<ObjectMissileLauncherFxEvent>
ObjectSimulationAbilityDomain::takeMissileLauncherFxEvents() {
    container::Vector<ObjectMissileLauncherFxEvent> result;
    result.swap(object_simulation_detail::state(*this).m_missileLauncherFxEvents);
    return result;
}

container::Vector<ObjectParticleUplinkPhaseEvent>
ObjectSimulationAbilityDomain::takeParticleUplinkPhaseEvents() {
    container::Vector<ObjectParticleUplinkPhaseEvent> result;
    result.swap(object_simulation_detail::state(*this).m_particleUplinkPhaseEvents);
    return result;
}

container::Vector<ObjectParticleUplinkBeamEvent>
ObjectSimulationAbilityDomain::takeParticleUplinkBeamEvents() {
    container::Vector<ObjectParticleUplinkBeamEvent> result;
    result.swap(object_simulation_detail::state(*this).m_particleUplinkBeamEvents);
    return result;
}

container::Vector<ObjectParticleUplinkScorchEvent>
ObjectSimulationAbilityDomain::takeParticleUplinkScorchEvents() {
    container::Vector<ObjectParticleUplinkScorchEvent> result;
    result.swap(object_simulation_detail::state(*this).m_particleUplinkScorchEvents);
    return result;
}

container::Vector<ObjectParticleUplinkFxEvent>
ObjectSimulationAbilityDomain::takeParticleUplinkFxEvents() {
    container::Vector<ObjectParticleUplinkFxEvent> result;
    result.swap(object_simulation_detail::state(*this).m_particleUplinkFxEvents);
    return result;
}

container::Vector<ObjectParticleUplinkRemnantSpawnRequest>
ObjectSimulationAbilityDomain::takeParticleUplinkRemnantSpawnRequests() {
    container::Vector<ObjectParticleUplinkRemnantSpawnRequest> result;
    result.swap(object_simulation_detail::state(*this).m_particleUplinkRemnantSpawnRequests);
    return result;
}

ObjectSimulationEventLease<ObjectParticleUplinkRemnantSpawnRequest>
ObjectSimulationAbilityDomain::leaseParticleUplinkRemnantSpawnRequests() {
    return ObjectSimulationEventLease<ObjectParticleUplinkRemnantSpawnRequest>{
        object_simulation_detail::state(*this)
            .m_particleUplinkRemnantSpawnRequests};
}

container::Vector<ObjectConstructionCompletionIntent>
ObjectSimulationConstructionDomain::takeCompletedObjectConstructions() {
    container::Vector<ObjectConstructionCompletionIntent> output =
        std::move(object_simulation_detail::state(*this).m_completedObjectConstructions);
    object_simulation_detail::state(*this).m_completedObjectConstructions.clear();
    return output;
}

ObjectSimulationEventLease<ObjectConstructionCompletionIntent>
ObjectSimulationConstructionDomain::leaseCompletedObjectConstructions() {
    return ObjectSimulationEventLease<ObjectConstructionCompletionIntent>{
        object_simulation_detail::state(*this)
            .m_completedObjectConstructions};
}

container::Vector<ObjectBridgeRepairScaffoldIntent>
ObjectSimulationConstructionDomain::takeBridgeRepairScaffoldIntents() {
    container::Vector<ObjectBridgeRepairScaffoldIntent> output;
    output.swap(object_simulation_detail::state(*this).m_bridgeRepairScaffoldIntents);
    return output;
}

ObjectSimulationEventLease<ObjectBridgeRepairScaffoldIntent>
ObjectSimulationConstructionDomain::leaseBridgeRepairScaffoldIntents() {
    return ObjectSimulationEventLease<ObjectBridgeRepairScaffoldIntent>{
        object_simulation_detail::state(*this)
            .m_bridgeRepairScaffoldIntents};
}

container::Vector<ObjectRebuildHoleExposeIntent>
ObjectSimulationConstructionDomain::takeRebuildHoleExposeIntents() {
    container::Vector<ObjectRebuildHoleExposeIntent> output;
    output.swap(object_simulation_detail::state(*this).m_rebuildExposeIntents);
    return output;
}

ObjectSimulationEventLease<ObjectRebuildHoleExposeIntent>
ObjectSimulationConstructionDomain::leaseRebuildHoleExposeIntents() {
    return ObjectSimulationEventLease<ObjectRebuildHoleExposeIntent>{
        object_simulation_detail::state(*this).m_rebuildExposeIntents};
}

container::Vector<ObjectRebuildWorkerSpawnIntent>
ObjectSimulationConstructionDomain::takeRebuildWorkerSpawnIntents() {
    container::Vector<ObjectRebuildWorkerSpawnIntent> output;
    output.swap(object_simulation_detail::state(*this).m_rebuildWorkerIntents);
    return output;
}

ObjectSimulationEventLease<ObjectRebuildWorkerSpawnIntent>
ObjectSimulationConstructionDomain::leaseRebuildWorkerSpawnIntents() {
    return ObjectSimulationEventLease<ObjectRebuildWorkerSpawnIntent>{
        object_simulation_detail::state(*this).m_rebuildWorkerIntents};
}

container::Vector<ObjectRebuildCompletionIntent>
ObjectSimulationConstructionDomain::takeRebuildCompletionIntents() {
    container::Vector<ObjectRebuildCompletionIntent> output;
    output.swap(object_simulation_detail::state(*this).m_rebuildCompletionIntents);
    return output;
}

ObjectSimulationEventLease<ObjectRebuildCompletionIntent>
ObjectSimulationConstructionDomain::leaseRebuildCompletionIntents() {
    return ObjectSimulationEventLease<ObjectRebuildCompletionIntent>{
        object_simulation_detail::state(*this).m_rebuildCompletionIntents};
}

container::Vector<ObjectRebuildTargetRemapIntent>
ObjectSimulationConstructionDomain::takeRebuildTargetRemapIntents() {
    container::Vector<ObjectRebuildTargetRemapIntent> output;
    output.swap(object_simulation_detail::state(*this).m_rebuildTargetRemapIntents);
    return output;
}

ObjectSimulationEventLease<ObjectRebuildTargetRemapIntent>
ObjectSimulationConstructionDomain::leaseRebuildTargetRemapIntents() {
    return ObjectSimulationEventLease<ObjectRebuildTargetRemapIntent>{
        object_simulation_detail::state(*this).m_rebuildTargetRemapIntents};
}

container::Vector<ObjectDeathEvent> ObjectSimulationLifecycleDomain::takeDeathEvents() {
    container::Vector<ObjectDeathEvent> result = std::move(object_simulation_detail::state(*this).m_deathEvents);
    object_simulation_detail::state(*this).m_deathEvents.clear();
    return result;
}

container::Vector<ObjectOwnershipChangeRequest>
ObjectSimulation::takeOwnershipChangeRequests() {
    auto& requests =
        object_simulation_detail::state(*this).m_ownershipChangeRequests;
    container::Vector<ObjectOwnershipChangeRequest> result =
        std::move(requests);
    requests.clear();
    return result;
}

ObjectSimulationEventLease<ObjectOwnershipChangeRequest>
ObjectSimulation::leaseOwnershipChangeRequests() {
    return ObjectSimulationEventLease<ObjectOwnershipChangeRequest>{
        object_simulation_detail::state(*this).m_ownershipChangeRequests};
}

void ObjectSimulation::discardConfirmedGameplayEvents() noexcept {
    auto& pending = object_simulation_detail::state(*this);

    pending.m_deleteWalks.clear();
    pending.m_deleteDestroyRequests.clear();
    pending.m_systemWeaponFireCommands.clear();
    pending.m_objectCreationListInvocations.clear();
    pending.m_objectReplacementInvocations.clear();
    pending.m_objectUpgradeFxInvocations.clear();
    pending.m_ownershipChangeRequests.clear();
    pending.m_objectDefectionRequests.clear();
    pending.m_pilotVehicleTakeoverRequests.clear();
    pending.m_railedTransportDockAttachCompletions.clear();
    pending.m_railroadDisembarkRequests.clear();
    pending.m_railroadCarriageSpawnRequests.clear();
    pending.m_spawnSlaveRequests.clear();
    pending.m_specialPowerSpawnRequests.clear();
    pending.m_bridgeStateEvents.clear();
    pending.m_completedObjectConstructions.clear();
    pending.m_bridgeRepairScaffoldIntents.clear();
    pending.m_rebuildTargetRemapIntents.clear();
    pending.m_rebuildExposeIntents.clear();
    pending.m_rebuildWorkerIntents.clear();
    pending.m_rebuildCompletionIntents.clear();
    pending.m_containmentEvents.clear();
    pending.m_vehicleNeutralizationRequests.clear();
    pending.m_cratePickupCommands.clear();
    pending.m_countermeasures.discardFlareSpawnCommands();
    pending.m_countermeasureFlareGameplayScratch.clear();
    pending.m_specialAbilityEffectRequests.clear();
    pending.m_specialAbilityFacingRequests.clear();
    pending.m_specialPowerCompletionEvents.clear();
    pending.m_structureEffectEvents.clear();
    pending.m_createObjectDieEvents.clear();
    pending.m_createCrateDieEvents.clear();

    // These three streams share records with the later presentation pass.
    // Drop only their authoritative payload, exactly as the takeGameplay
    // accessors did before the transaction chain was aborted.
    pending.m_transitionDamageFxEvents.erase(
        std::remove_if(
            pending.m_transitionDamageFxEvents.begin(),
            pending.m_transitionDamageFxEvents.end(),
            [](const ObjectTransitionDamageFxEvent& event) {
                return event.kind ==
                    ObjectTransitionDamageFxEventKind::ObjectCreationList;
            }),
        pending.m_transitionDamageFxEvents.end());
    pending.m_transitionDamageGameplayScratch.clear();
    for (ObjectInstantDeathEffectEvent& event :
         pending.m_instantDeathEffectEvents) {
        event.ocl.reset();
        event.weapon.reset();
    }
    pending.m_instantDeathGameplayScratch.clear();
    for (ObjectSlowDeathPhaseEvent& event : pending.m_slowDeathPhaseEvents) {
        event.ocl.reset();
        event.weapon.reset();
        event.rubbleObject.reset();
    }
    pending.m_slowDeathGameplayScratch.clear();

    pending.m_tactical.discardToppleGameplayRequests();
    pending.m_toppleStumpGameplayScratch.clear();
    pending.m_topplePathfindGameplayScratch.clear();
    pending.m_physicsCrashCommands.clear();
    pending.m_aiMovementObstructionEvents.clear();
    pending.m_mineSpawnCommands.clear();
    pending.m_particleUplinkRemnantSpawnRequests.clear();
    pending.m_waveGuideBridgeImpacts.clear();
    pending.m_checkpointNavigationEvents.clear();
    pending.m_tensileNavigationEvents.clear();
    pending.m_dynamicGeometryGameplayEvents.clear();
    pending.m_transportEvents.gameplay.clear();
    pending.m_transportEvents.presentation.clear();
}

container::Vector<ObjectSpecialPowerCompletionEvent>
ObjectSimulationLifecycleDomain::takeSpecialPowerCompletionEvents() {
    container::Vector<ObjectSpecialPowerCompletionEvent> result =
        std::move(object_simulation_detail::state(*this).m_specialPowerCompletionEvents);
    object_simulation_detail::state(*this).m_specialPowerCompletionEvents.clear();
    return result;
}

ObjectSimulationEventLease<ObjectSpecialPowerCompletionEvent>
ObjectSimulationLifecycleDomain::leaseSpecialPowerCompletionEvents() {
    return ObjectSimulationEventLease<ObjectSpecialPowerCompletionEvent>{
        object_simulation_detail::state(*this)
            .m_specialPowerCompletionEvents};
}

container::Vector<ObjectVehicleNeutralizationRequest>
ObjectSimulationLifecycleDomain::takeVehicleNeutralizationRequests() {
    container::Vector<ObjectVehicleNeutralizationRequest> result =
        std::move(object_simulation_detail::state(*this)
                      .m_vehicleNeutralizationRequests);
    object_simulation_detail::state(*this)
        .m_vehicleNeutralizationRequests.clear();
    ObjectSimulation& simulation = static_cast<ObjectSimulation&>(*this);
    for (ObjectVehicleNeutralizationRequest& event : result) {
        if (event.submissionOrdinal == 0) {
            event.submissionOrdinal =
                simulation.reserveGameplaySubmissionOrdinal();
        }
    }
    return result;
}

ObjectSimulationEventLease<ObjectVehicleNeutralizationRequest>
ObjectSimulationLifecycleDomain::leaseVehicleNeutralizationRequests() {
    ObjectSimulationEventLease<ObjectVehicleNeutralizationRequest> result{
        object_simulation_detail::state(*this)
            .m_vehicleNeutralizationRequests};
    ObjectSimulation& simulation = static_cast<ObjectSimulation&>(*this);
    for (ObjectVehicleNeutralizationRequest& event : result) {
        if (event.submissionOrdinal == 0) {
            event.submissionOrdinal =
                simulation.reserveGameplaySubmissionOrdinal();
        }
    }
    return result;
}

container::Vector<ObjectCrushDieEvent> ObjectSimulationLifecycleDomain::takeCrushDieEvents() {
    container::Vector<ObjectCrushDieEvent> result = std::move(object_simulation_detail::state(*this).m_crushDieEvents);
    object_simulation_detail::state(*this).m_crushDieEvents.clear();
    return result;
}

container::Vector<ObjectInstantDeathEffectEvent> ObjectSimulationLifecycleDomain::takeInstantDeathEffectEvents() {
    container::Vector<ObjectInstantDeathEffectEvent> result = std::move(object_simulation_detail::state(*this).m_instantDeathEffectEvents);
    object_simulation_detail::state(*this).m_instantDeathEffectEvents.clear();
    return result;
}

container::Vector<ObjectInstantDeathEffectEvent>
ObjectSimulationLifecycleDomain::takeInstantDeathGameplayEvents() {
    auto leased = leaseInstantDeathGameplayEvents();
    container::Vector<ObjectInstantDeathEffectEvent> output;
    output.reserve(leased.size());
    for (ObjectInstantDeathEffectEvent& event : leased) {
        output.push_back(std::move(event));
    }
    return output;
}

ObjectSimulationEventLease<ObjectInstantDeathEffectEvent>
ObjectSimulationLifecycleDomain::leaseInstantDeathGameplayEvents() {
    auto& state = object_simulation_detail::state(*this);
    auto& source = state.m_instantDeathEffectEvents;
    auto& gameplay = state.m_instantDeathGameplayScratch;
    gameplay.clear();
    gameplay.reserve(source.size());
    for (ObjectInstantDeathEffectEvent& event : source) {
        if (!event.ocl || event.ocl->empty()) {
            if (!event.weapon || event.weapon->empty()) continue;
        }
        ObjectInstantDeathEffectEvent payload = event;
        payload.fx.reset();
        gameplay.push_back(std::move(payload));
        event.ocl.reset();
        event.weapon.reset();
    }
    return ObjectSimulationEventLease<ObjectInstantDeathEffectEvent>{gameplay};
}

container::Vector<ObjectCreateObjectDieEvent>
ObjectSimulationLifecycleDomain::takeCreateObjectDieEvents() {
    container::Vector<ObjectCreateObjectDieEvent> result =
        std::move(object_simulation_detail::state(*this).m_createObjectDieEvents);
    object_simulation_detail::state(*this).m_createObjectDieEvents.clear();
    return result;
}

ObjectSimulationEventLease<ObjectCreateObjectDieEvent>
ObjectSimulationLifecycleDomain::leaseCreateObjectDieEvents() {
    return ObjectSimulationEventLease<ObjectCreateObjectDieEvent>{
        object_simulation_detail::state(*this).m_createObjectDieEvents};
}

container::Vector<ObjectCreateCrateDieEvent>
ObjectSimulationLifecycleDomain::takeCreateCrateDieEvents() {
    container::Vector<ObjectCreateCrateDieEvent> result =
        std::move(object_simulation_detail::state(*this).m_createCrateDieEvents);
    object_simulation_detail::state(*this).m_createCrateDieEvents.clear();
    return result;
}

ObjectSimulationEventLease<ObjectCreateCrateDieEvent>
ObjectSimulationLifecycleDomain::leaseCreateCrateDieEvents() {
    return ObjectSimulationEventLease<ObjectCreateCrateDieEvent>{
        object_simulation_detail::state(*this).m_createCrateDieEvents};
}

container::Vector<ObjectFxListDieEffectEvent> ObjectSimulationLifecycleDomain::takeFxListDieEffectEvents() {
    container::Vector<ObjectFxListDieEffectEvent> result = std::move(object_simulation_detail::state(*this).m_fxListDieEffectEvents);
    object_simulation_detail::state(*this).m_fxListDieEffectEvents.clear();
    return result;
}

container::Vector<ObjectHeightDiePresentationEvent> ObjectSimulationLifecycleDomain::takeHeightDiePresentationEvents() {
    container::Vector<ObjectHeightDiePresentationEvent> result = std::move(object_simulation_detail::state(*this).m_heightDiePresentationEvents);
    object_simulation_detail::state(*this).m_heightDiePresentationEvents.clear();
    return result;
}

container::Vector<ObjectSlowDeathPhaseEvent> ObjectSimulationLifecycleDomain::takeSlowDeathPhaseEvents() {
    container::Vector<ObjectSlowDeathPhaseEvent> result = std::move(object_simulation_detail::state(*this).m_slowDeathPhaseEvents);
    object_simulation_detail::state(*this).m_slowDeathPhaseEvents.clear();
    return result;
}

container::Vector<ObjectSlowDeathPhaseEvent>
ObjectSimulationLifecycleDomain::takeSlowDeathGameplayEvents() {
    auto leased = leaseSlowDeathGameplayEvents();
    container::Vector<ObjectSlowDeathPhaseEvent> output;
    output.reserve(leased.size());
    for (ObjectSlowDeathPhaseEvent& event : leased) {
        output.push_back(std::move(event));
    }
    return output;
}

ObjectSimulationEventLease<ObjectSlowDeathPhaseEvent>
ObjectSimulationLifecycleDomain::leaseSlowDeathGameplayEvents() {
    auto& state = object_simulation_detail::state(*this);
    auto& source = state.m_slowDeathPhaseEvents;
    auto& gameplay = state.m_slowDeathGameplayScratch;
    gameplay.clear();
    gameplay.reserve(source.size());
    for (ObjectSlowDeathPhaseEvent& event : source) {
        const bool hasOcl = event.ocl && !event.ocl->empty();
        const bool hasWeapon = event.weapon && !event.weapon->empty();
        const bool hasRubble =
            event.rubbleObject && !event.rubbleObject->empty();
        if (!hasOcl && !hasWeapon && !hasRubble) continue;
        ObjectSlowDeathPhaseEvent payload = event;
        payload.fx.reset();
        gameplay.push_back(std::move(payload));
        event.ocl.reset();
        event.weapon.reset();
        event.rubbleObject.reset();
    }
    return ObjectSimulationEventLease<ObjectSlowDeathPhaseEvent>{gameplay};
}

container::Vector<ObjectMovementEvent> ObjectSimulationMotionDomain::takeMovementEvents() {
    container::Vector<ObjectMovementEvent> result = std::move(object_simulation_detail::state(*this).m_movementEvents);
    object_simulation_detail::state(*this).m_movementEvents.clear();
    return result;
}

container::Vector<ai::MovementFeedback>
ObjectSimulationMotionDomain::takeAIMovementFeedback() {
    container::Vector<ai::MovementFeedback> result =
        std::move(object_simulation_detail::state(*this).m_aiMovementFeedback);
    object_simulation_detail::state(*this).m_aiMovementFeedback.clear();
    return result;
}

void ObjectSimulationMotionDomain::drainAIMovementFeedback(
    container::Vector<ai::MovementFeedback>& out) {
    auto& source =
        object_simulation_detail::state(*this).m_aiMovementFeedback;
    out.clear();
    out.reserve(source.size());
    out.insert(out.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end()));
    source.clear();
}

container::Vector<ai::AIFacingFeedback>
ObjectSimulationMotionDomain::takeAIFacingFeedback() {
    container::Vector<ai::AIFacingFeedback> result =
        std::move(object_simulation_detail::state(*this).m_aiFacingFeedback);
    object_simulation_detail::state(*this).m_aiFacingFeedback.clear();
    return result;
}

void ObjectSimulationMotionDomain::drainAIFacingFeedback(
    container::Vector<ai::AIFacingFeedback>& out) {
    auto& source = object_simulation_detail::state(*this).m_aiFacingFeedback;
    out.clear();
    out.reserve(source.size());
    out.insert(out.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end()));
    source.clear();
}

container::Vector<ObjectPhysicsEvent> ObjectSimulationMotionDomain::takePhysicsEvents() {
    container::Vector<ObjectPhysicsEvent> result = std::move(object_simulation_detail::state(*this).m_physicsEvents);
    object_simulation_detail::state(*this).m_physicsEvents.clear();
    return result;
}

container::Vector<ObjectAIMovementObstructionEvent>
ObjectSimulationMotionDomain::takeAIMovementObstructionEvents() {
    container::Vector<ObjectAIMovementObstructionEvent> result = std::move(
        object_simulation_detail::state(*this)
            .m_aiMovementObstructionEvents);
    object_simulation_detail::state(*this)
        .m_aiMovementObstructionEvents.clear();
    return result;
}

ObjectSimulationEventLease<ObjectAIMovementObstructionEvent>
ObjectSimulationMotionDomain::leaseAIMovementObstructionEvents() {
    return ObjectSimulationEventLease<ObjectAIMovementObstructionEvent>{
        object_simulation_detail::state(*this).m_aiMovementObstructionEvents};
}

container::Vector<ObjectPhysicsCrashCommand>
ObjectSimulationMotionDomain::takePhysicsCrashCommands() {
    container::Vector<ObjectPhysicsCrashCommand> result =
        std::move(object_simulation_detail::state(*this).m_physicsCrashCommands);
    object_simulation_detail::state(*this).m_physicsCrashCommands.clear();
    return result;
}

ObjectSimulationEventLease<ObjectPhysicsCrashCommand>
ObjectSimulationMotionDomain::leasePhysicsCrashCommands() {
    return ObjectSimulationEventLease<ObjectPhysicsCrashCommand>{
        object_simulation_detail::state(*this).m_physicsCrashCommands};
}

container::Vector<ObjectExperienceEvent> ObjectSimulationProgressionDomain::takeExperienceEvents() {
    container::Vector<ObjectExperienceEvent> result = std::move(object_simulation_detail::state(*this).m_experienceEvents);
    object_simulation_detail::state(*this).m_experienceEvents.clear();
    return result;
}

container::Vector<ObjectAutoDepositEvent> ObjectSimulationProgressionDomain::takeAutoDepositEvents() {
    container::Vector<ObjectAutoDepositEvent> result = std::move(object_simulation_detail::state(*this).m_autoDepositEvents);
    object_simulation_detail::state(*this).m_autoDepositEvents.clear();
    return result;
}

container::Vector<ObjectSupplyEvent> ObjectSimulationProgressionDomain::takeSupplyEvents() {
    container::Vector<ObjectSupplyEvent> result = std::move(object_simulation_detail::state(*this).m_supplyEvents);
    object_simulation_detail::state(*this).m_supplyEvents.clear();
    return result;
}

container::Vector<ObjectCratePickupCommand> ObjectSimulationProgressionDomain::takeCratePickupCommands() {
    container::Vector<ObjectCratePickupCommand> result = std::move(object_simulation_detail::state(*this).m_cratePickupCommands);
    object_simulation_detail::state(*this).m_cratePickupCommands.clear();
    return result;
}

ObjectSimulationEventLease<ObjectCratePickupCommand>
ObjectSimulationProgressionDomain::leaseCratePickupCommands() {
    return ObjectSimulationEventLease<ObjectCratePickupCommand>{
        object_simulation_detail::state(*this).m_cratePickupCommands};
}

container::Vector<ObjectContainmentEvent>
ObjectSimulationContainmentDomain::takeContainmentEvents() {
    container::Vector<ObjectContainmentEvent> result =
        std::move(object_simulation_detail::state(*this).m_containmentEvents);
    object_simulation_detail::state(*this).m_containmentEvents.clear();
    ObjectSimulation& simulation = static_cast<ObjectSimulation&>(*this);
    for (ObjectContainmentEvent& event : result) {
        if (event.submissionOrdinal == 0) {
            event.submissionOrdinal =
                simulation.reserveGameplaySubmissionOrdinal();
        }
    }
    return result;
}

ObjectSimulationEventLease<ObjectContainmentEvent>
ObjectSimulationContainmentDomain::leaseContainmentEvents() {
    ObjectSimulationEventLease<ObjectContainmentEvent> result{
        object_simulation_detail::state(*this).m_containmentEvents};
    ObjectSimulation& simulation = static_cast<ObjectSimulation&>(*this);
    for (ObjectContainmentEvent& event : result) {
        if (event.submissionOrdinal == 0) {
            event.submissionOrdinal =
                simulation.reserveGameplaySubmissionOrdinal();
        }
    }
    return result;
}

container::Vector<ObjectTransportGameplayTransaction>
ObjectSimulationContainmentDomain::takeTransportGameplayTransactions() {
    auto& pending = object_simulation_detail::state(*this)
                        .m_transportEvents.gameplay;
    container::Vector<ObjectTransportGameplayTransaction> result =
        std::move(pending);
    pending.clear();
    return result;
}

ObjectSimulationEventLease<ObjectTransportGameplayTransaction>
ObjectSimulationContainmentDomain::leaseTransportGameplayTransactions() {
    return ObjectSimulationEventLease<ObjectTransportGameplayTransaction>{
        object_simulation_detail::state(*this).m_transportEvents.gameplay};
}

container::Vector<ObjectTransportPresentationEvent>
ObjectSimulationContainmentDomain::takeTransportPresentationEvents() {
    auto& pending = object_simulation_detail::state(*this)
                        .m_transportEvents.presentation;
    container::Vector<ObjectTransportPresentationEvent> result =
        std::move(pending);
    pending.clear();
    return result;
}

container::Vector<ObjectWeaponBonusUpdateEvent>
ObjectSimulationProgressionDomain::takeWeaponBonusUpdateEvents() {
    container::Vector<ObjectWeaponBonusUpdateEvent> result =
        std::move(object_simulation_detail::state(*this).m_weaponBonusUpdateEvents);
    object_simulation_detail::state(*this).m_weaponBonusUpdateEvents.clear();
    return result;
}

container::Vector<ObjectBridgeStateEvent>
ObjectSimulationConstructionDomain::takeObjectBridgeStateEvents() {
    container::Vector<ObjectBridgeStateEvent> result;
    result.swap(object_simulation_detail::state(*this).m_bridgeStateEvents);
    return result;
}

ObjectSimulationEventLease<ObjectBridgeStateEvent>
ObjectSimulationConstructionDomain::leaseObjectBridgeStateEvents() {
    return ObjectSimulationEventLease<ObjectBridgeStateEvent>{
        object_simulation_detail::state(*this).m_bridgeStateEvents};
}

container::Vector<ObjectRailedTransportDockAttachCompletion>
ObjectSimulationConstructionDomain::takeRailedTransportDockAttachCompletions() {
    container::Vector<ObjectRailedTransportDockAttachCompletion> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_railedTransportDockAttachCompletions);
    return result;
}

ObjectSimulationEventLease<ObjectRailedTransportDockAttachCompletion>
ObjectSimulationConstructionDomain::leaseRailedTransportDockAttachCompletions() {
    return ObjectSimulationEventLease<ObjectRailedTransportDockAttachCompletion>{
        object_simulation_detail::state(*this)
            .m_railedTransportDockAttachCompletions};
}

container::Vector<ObjectRailroadCarriageSpawnRequest>
ObjectSimulationConstructionDomain::takeRailroadCarriageSpawnRequests() {
    container::Vector<ObjectRailroadCarriageSpawnRequest> result;
    result.swap(object_simulation_detail::state(*this).m_railroadCarriageSpawnRequests);
    return result;
}

ObjectSimulationEventLease<ObjectRailroadCarriageSpawnRequest>
ObjectSimulationConstructionDomain::leaseRailroadCarriageSpawnRequests() {
    return ObjectSimulationEventLease<ObjectRailroadCarriageSpawnRequest>{
        object_simulation_detail::state(*this)
            .m_railroadCarriageSpawnRequests};
}

container::Vector<ObjectRailroadDisembarkRequest>
ObjectSimulationConstructionDomain::takeRailroadDisembarkRequests() {
    container::Vector<ObjectRailroadDisembarkRequest> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_railroadDisembarkRequests);
    return result;
}

container::Vector<ObjectRailroadPresentationEvent>
ObjectSimulationConstructionDomain::takeRailroadPresentationEvents() {
    container::Vector<ObjectRailroadPresentationEvent> result;
    result.swap(object_simulation_detail::state(*this)
                    .m_railroadPresentationEvents);
    return result;
}

ObjectSimulationEventLease<ObjectRailroadDisembarkRequest>
ObjectSimulationConstructionDomain::leaseRailroadDisembarkRequests() {
    return ObjectSimulationEventLease<ObjectRailroadDisembarkRequest>{
        object_simulation_detail::state(*this).m_railroadDisembarkRequests};
}

container::Vector<ObjectSpawnSlaveRequest>
ObjectSimulationConstructionDomain::takeObjectSpawnSlaveRequests() {
    container::Vector<ObjectSpawnSlaveRequest> result;
    result.swap(object_simulation_detail::state(*this).m_spawnSlaveRequests);
    return result;
}

ObjectSimulationEventLease<ObjectSpawnSlaveRequest>
ObjectSimulationConstructionDomain::leaseObjectSpawnSlaveRequests() {
    return ObjectSimulationEventLease<ObjectSpawnSlaveRequest>{
        object_simulation_detail::state(*this).m_spawnSlaveRequests};
}

container::Vector<ObjectSlaveRepairPresentationEvent>
ObjectSimulationConstructionDomain::takeObjectSlaveRepairPresentationEvents() {
    container::Vector<ObjectSlaveRepairPresentationEvent> result;
    result.swap(object_simulation_detail::state(*this).m_slaveRepairPresentationEvents);
    return result;
}

container::Vector<ObjectTensileFormationEvent>
ObjectSimulationConstructionDomain::takeObjectTensileNavigationEvents() {
    container::Vector<ObjectTensileFormationEvent> result;
    result.swap(
        object_simulation_detail::state(*this).m_tensileNavigationEvents);
    return result;
}

ObjectSimulationEventLease<ObjectTensileFormationEvent>
ObjectSimulationConstructionDomain::leaseObjectTensileNavigationEvents() {
    return ObjectSimulationEventLease<ObjectTensileFormationEvent>{
        object_simulation_detail::state(*this).m_tensileNavigationEvents};
}

container::Vector<ObjectTensileFormationEvent>
ObjectSimulationConstructionDomain::takeObjectTensileFormationEvents() {
    container::Vector<ObjectTensileFormationEvent> result;
    result.swap(object_simulation_detail::state(*this).m_tensileFormationEvents);
    return result;
}

container::Vector<ObjectSystemWeaponFireCommand>
ObjectSimulationAbilityDomain::takeSystemWeaponFireCommands() {
    container::Vector<ObjectSystemWeaponFireCommand> result =
        std::move(object_simulation_detail::state(*this).m_systemWeaponFireCommands);
    object_simulation_detail::state(*this).m_systemWeaponFireCommands.clear();
    return result;
}

ObjectSimulationEventLease<ObjectSystemWeaponFireCommand>
ObjectSimulationAbilityDomain::leaseSystemWeaponFireCommands() {
    return ObjectSimulationEventLease<ObjectSystemWeaponFireCommand>{
        object_simulation_detail::state(*this).m_systemWeaponFireCommands};
}

container::Vector<ObjectCreationListInvocation>
ObjectSimulationLifecycleDomain::takeObjectCreationListInvocations() {
    container::Vector<ObjectCreationListInvocation> result =
        std::move(object_simulation_detail::state(*this).m_objectCreationListInvocations);
    object_simulation_detail::state(*this).m_objectCreationListInvocations.clear();
    return result;
}

ObjectSimulationEventLease<ObjectCreationListInvocation>
ObjectSimulationLifecycleDomain::leaseObjectCreationListInvocations() {
    return ObjectSimulationEventLease<ObjectCreationListInvocation>{
        object_simulation_detail::state(*this)
            .m_objectCreationListInvocations};
}

container::Vector<ObjectReplacementInvocation>
ObjectSimulationLifecycleDomain::takeObjectReplacementInvocations() {
    container::Vector<ObjectReplacementInvocation> result =
        std::move(object_simulation_detail::state(*this).m_objectReplacementInvocations);
    object_simulation_detail::state(*this).m_objectReplacementInvocations.clear();
    return result;
}

ObjectSimulationEventLease<ObjectReplacementInvocation>
ObjectSimulationLifecycleDomain::leaseObjectReplacementInvocations() {
    return ObjectSimulationEventLease<ObjectReplacementInvocation>{
        object_simulation_detail::state(*this)
            .m_objectReplacementInvocations};
}

container::Vector<ObjectUpgradeFxInvocation>
ObjectSimulationProgressionDomain::takeObjectUpgradeFxInvocations() {
    container::Vector<ObjectUpgradeFxInvocation> result =
        std::move(object_simulation_detail::state(*this).m_objectUpgradeFxInvocations);
    object_simulation_detail::state(*this).m_objectUpgradeFxInvocations.clear();
    return result;
}

ObjectSimulationEventLease<ObjectUpgradeFxInvocation>
ObjectSimulationProgressionDomain::leaseObjectUpgradeFxInvocations() {
    return ObjectSimulationEventLease<ObjectUpgradeFxInvocation>{
        object_simulation_detail::state(*this).m_objectUpgradeFxInvocations};
}

container::Vector<ObjectFireAudioCommand>
ObjectSimulationAbilityDomain::takeObjectFireAudioCommands() {
    container::Vector<ObjectFireAudioCommand> result =
        std::move(object_simulation_detail::state(*this).m_objectFireAudioCommands);
    object_simulation_detail::state(*this).m_objectFireAudioCommands.clear();
    return result;
}

void ObjectSimulation::queueSystemWeaponFireCommand(
    ObjectSystemWeaponFireCommand command) {
    if (command.emissionSequence == 0) {
        command.emissionSequence = reserveGameplaySubmissionOrdinal();
    }
    object_simulation_detail::state(*this).m_systemWeaponFireCommands.push_back(std::move(command));
}

void ObjectSimulation::queueObjectCreationListInvocation(
    ObjectCreationListInvocation invocation) {
    if (invocation.emissionSequence == 0) {
        invocation.emissionSequence = reserveGameplaySubmissionOrdinal();
    }
    object_simulation_detail::state(*this).m_objectCreationListInvocations.push_back(std::move(invocation));
}

void ObjectSimulation::queueObjectReplacementInvocation(
    ObjectReplacementInvocation invocation) {
    if (invocation.emissionSequence == 0) {
        invocation.emissionSequence = reserveGameplaySubmissionOrdinal();
    }
    object_simulation_detail::state(*this).m_objectReplacementInvocations.push_back(std::move(invocation));
}

void ObjectSimulation::queueObjectUpgradeFxInvocation(
    ObjectUpgradeFxInvocation invocation) {
    if (invocation.emissionSequence == 0) {
        invocation.emissionSequence = reserveGameplaySubmissionOrdinal();
    }
    object_simulation_detail::state(*this).m_objectUpgradeFxInvocations.push_back(std::move(invocation));
}

uint64_t ObjectSimulation::reserveGameplaySubmissionOrdinal() noexcept {
    const uint64_t result = object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal;
    ++object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal;
    if (object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal == 0) {
        ++object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal;
    }
    return result;
}

} // namespace engine
