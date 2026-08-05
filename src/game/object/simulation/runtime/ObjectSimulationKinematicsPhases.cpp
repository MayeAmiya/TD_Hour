#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/terrain/TerrainLogic.h"

#include <utility>

namespace engine {

void ObjectSimulation::updateKinematicsPreludePhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    resolveQueuedPhysics(registry, lifecycle, confirmedTick);
    updateKinematicsSmartBombs(
        registry, lifecycle, terrain, confirmedTick);
    updateWaveGuideKinematicsPhase(
        registry, lifecycle, terrain, confirmedTick, context);
}

ObjectKinematicsPhaseState ObjectSimulation::updateKinematicsMotionPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context,
    container::Span<const ObjectId> aiMoveStopOwners,
    container::Span<const ObjectId> aiAttackOwners,
    container::Span<const ObjectAIMovementCommand> aiMovementCommands,
    container::Span<const ai::AIStateCommand> aiFacingCommands) {
    object_simulation_detail::updateMovement(
        registry, lifecycle, terrain, confirmedTick,
        object_simulation_detail::state(*this).m_rules,
        object_simulation_detail::state(*this).m_movementEvents,
        object_simulation_detail::state(*this).m_aiMovementFeedback,
        aiMoveStopOwners, aiAttackOwners, aiMovementCommands,
        context.navigation);
    object_simulation_detail::updateAIFacing(
        registry, lifecycle, terrain,
        object_simulation_detail::state(*this).m_rules, confirmedTick,
        aiFacingCommands,
        object_simulation_detail::state(*this).m_aiFacingFeedback);

    auto& physicsDamage =
        object_simulation_detail::state(*this).m_damageScratch;
    physicsDamage.clear();
    const size_t physicsEventBegin =
        object_simulation_detail::state(*this).m_physicsEvents.size();
    object_simulation_detail::updatePhysics(
        registry, lifecycle, terrain,
        object_simulation_detail::state(*this).m_rules, confirmedTick,
        object_simulation_detail::state(*this).m_physicsEvents,
        physicsDamage,
        object_simulation_detail::state(*this).m_physicsScratch);
    for (ObjectDamageRequest& request : physicsDamage) {
        queueDamage(std::move(request));
    }
    physicsDamage.clear();
    object_simulation_detail::resolvePhysicsCollisions(
        registry, lifecycle, terrain, context.players, context.navigation,
        object_simulation_detail::state(*this).m_rules, confirmedTick,
        object_simulation_detail::state(*this)
            .m_nextGameplaySubmissionOrdinal,
        physicsDamage,
        object_simulation_detail::state(*this)
            .m_pilotVehicleTakeoverRequests,
        object_simulation_detail::state(*this).m_physicsCrashCommands,
        object_simulation_detail::state(*this)
            .m_aiMovementObstructionEvents,
        object_simulation_detail::state(*this).m_physicsScratch,
        object_simulation_detail::state(*this).m_contactIndex);
    for (ObjectDamageRequest& request : physicsDamage) {
        queueDamage(std::move(request));
    }
    updateKinematicsFloats(registry, lifecycle, terrain, confirmedTick);
    object_simulation_detail::state(*this)
        .m_containment.synchronizeTransforms(
            registry, lifecycle, context.content);
    updateKinematicsStickyBombs(
        registry, lifecycle, terrain, confirmedTick);
    updateKinematicsContainment(
        registry, lifecycle, terrain, confirmedTick, context);
    queueKinematicsHeightDeaths(
        registry, lifecycle, terrain, confirmedTick);
    return {.physicsEventBegin = physicsEventBegin};
}

void ObjectSimulation::finishKinematicsPostDamagePhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectKinematicsPhaseState phase) {
    auto& simulationState = object_simulation_detail::state(*this);
    for (size_t index = phase.physicsEventBegin;
         index < simulationState.m_physicsEvents.size(); ++index) {
        const ObjectPhysicsEvent& event = simulationState.m_physicsEvents[index];
        if (event.kind == ObjectPhysicsEventKind::Landed) {
            static_cast<void>(simulationState.m_airfield.notifyAircraftHitGround(
                registry, lifecycle, simulationState.m_rules, event.object,
                confirmedTick, simulationState.m_airfieldEvents,
                simulationState.m_slowDeathPhaseEvents,
                simulationState.m_deleteDestroyRequests,
                simulationState.m_nextGameplaySubmissionOrdinal));
        }
    }
    object_simulation_detail::updateSlowDeaths(
        registry, lifecycle, &terrain, simulationState.m_rules,
        confirmedTick, simulationState.m_slowDeathPhaseEvents,
        simulationState.m_nextGameplaySubmissionOrdinal);
    object_simulation_detail::updateSubdualRecovery(
        registry, lifecycle, simulationState.m_rules, confirmedTick,
        simulationState.m_healthEvents);
    object_simulation_detail::updateTimedStatusDamage(
        registry, confirmedTick);
    updateKinematicsAnimationSteering(
        registry, lifecycle, confirmedTick);
}

void ObjectSimulation::finishKinematicsCollisionPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    finishKinematicsFireWeaponCollisions(
        registry, lifecycle, confirmedTick, context);
    finishKinematicsCrateCollisions(
        registry, lifecycle, terrain, confirmedTick, context);
}

} // namespace engine
