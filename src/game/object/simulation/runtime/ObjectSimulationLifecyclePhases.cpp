#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/terrain/TerrainLogic.h"

namespace engine {

void ObjectSimulation::prepareLifecyclePhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context,
    container::Vector<ObjectLifetimeCommand>& commands) {
    auto& simulationState = object_simulation_detail::state(*this);
    simulationState.m_bridge.updateDeathEffects(
        confirmedTick, simulationState.m_bridgeDeathEffects,
        simulationState.m_structureEffectEvents,
        simulationState.m_objectCreationListInvocations);
    container::Vector<ObjectBoneFxStopRequest> structureBoneStops;
    simulationState.m_structureDestruction.update(
        registry, lifecycle, terrain, context.content, context.random,
        simulationState.m_rules, confirmedTick,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_structureEffectEvents,
        simulationState.m_systemWeaponFireCommands, structureBoneStops);
    if (context.players && context.spatialIndex) {
        simulationState.m_stealth.updateGrantors(
            registry, lifecycle, *context.players, *context.spatialIndex,
            confirmedTick, simulationState.m_grantStealthPulseEvents);
        simulationState.m_stealth.updateDetectors(
            registry, lifecycle, *context.players, *context.spatialIndex,
            simulationState.m_rules, confirmedTick,
            simulationState.m_stealthDetectorPulseEvents);
    }
    simulationState.m_stealth.update(
        registry, lifecycle, simulationState.m_rules, confirmedTick);
    for (const ObjectBoneFxStopRequest& request : structureBoneStops) {
        static_cast<void>(simulationState.m_boneFx.stopAll(
            registry, lifecycle, request,
            simulationState.m_nextGameplaySubmissionOrdinal,
            simulationState.m_transitionDamageFxEvents));
    }
    simulationState.m_boneFx.update(
        registry, lifecycle, context.content, simulationState.m_rules,
        simulationState.m_sessionSeed, confirmedTick,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_transitionDamageFxEvents);

    commands.clear();
    simulationState.m_lifetime.update(
        registry, lifecycle, simulationState.m_rules,
        simulationState.m_sessionSeed, confirmedTick, commands);
}

bool ObjectSimulation::applyLifetimeCommand(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectLifetimeCommand& command, uint64_t confirmedTick) {
    if (!command.object || !lifecycle.entityFromId(command.object) ||
        lifecycle.isPendingDestroy(command.object)) {
        return false;
    }
    if (command.action == game::ObjectLifetimeAction::Destroy) {
        static_cast<void>(lifecycle.requestDestroy(
            command.object, ObjectDestroyReason::System,
            confirmedTick));
        return false;
    }
    queueDamage({
        .target = command.object,
        .sourceSequence = command.authoredOrder,
        .causalGroup = command.object,
        .damageType = game::DamageType::UNRESISTABLE,
        .deathType = game::DeathType::NORMAL,
        .forceKill = true,
        .confirmedTick = confirmedTick,
    });
    return true;
}

void ObjectSimulation::finishLifecyclePhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick) {
    auto& simulationState = object_simulation_detail::state(*this);
    object_simulation_detail::updateSlowDeaths(
        registry, lifecycle, &terrain, simulationState.m_rules,
        confirmedTick, simulationState.m_slowDeathPhaseEvents,
        simulationState.m_nextGameplaySubmissionOrdinal);
}

} // namespace engine
