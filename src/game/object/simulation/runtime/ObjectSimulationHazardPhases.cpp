#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"

#include "game/base/SimulationRandom.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/TerrainLogic.h"

#include <utility>

namespace engine {

void ObjectSimulation::updateFrameAdmissionPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
    updateObjectTerrainDecals(registry, confirmedTick);
    if (context.players && context.content) {
        object_simulation_detail::state(*this)
            .m_specialPower.updatePassivePlayerEffects(
                registry, lifecycle, *context.players, *context.content);
    }
    object_simulation_detail::state(*this)
        .m_specialPower.updateDefectionDetection(
            registry, lifecycle, confirmedTick);
    if (context.players && context.content && context.random) {
        updateWeaponBonuses(
            registry, lifecycle, *context.players, *context.content,
            *context.random, confirmedTick);
    }
}

bool ObjectSimulation::updateNeutronSlowDeathPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context,
    container::Vector<ObjectDamageRequest>& outDamage) {
    if (!context.content || !context.random) return false;
    auto& simulationState = object_simulation_detail::state(*this);
    return simulationState.m_neutronMissileSlowDeath.update(
        registry, lifecycle, terrain, simulationState.m_rules,
        confirmedTick, outDamage,
        simulationState.m_neutronMissilePresentationEvents);
}

void ObjectSimulation::updateMinefieldHazardPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    if (!context.players || !context.content || !context.random) return;
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_minefield.update(
        registry, lifecycle, *context.players, *context.content, terrain,
        context.spatialIndex, *context.random, simulationState.m_rules,
        confirmedTick, simulationState.m_nextGameplaySubmissionOrdinal,
        damage, simulationState.m_systemWeaponFireCommands,
        simulationState.m_objectCreationListInvocations,
        simulationState.m_mineSpawnCommands,
        simulationState.m_minefieldFxEvents);
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
}

void ObjectSimulation::updateDynamicGeometryHazardPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    game::terrain::TerrainLogic& terrain, uint64_t confirmedTick) {
    auto& simulationState = object_simulation_detail::state(*this);
    simulationState.m_dynamicGeometry.update(
        registry, lifecycle, terrain, confirmedTick,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_dynamicGeometryGameplayEvents,
        simulationState.m_dynamicGeometryPresentationEvents);
}

void ObjectSimulation::updateFlammableHazardPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_fireUpdates.updateFlammable(
        registry, lifecycle, simulationState.m_rules.logicFramesPerSecond,
        confirmedTick, damage, simulationState.m_objectFireAudioCommands);
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
}

void ObjectSimulation::updatePoisonHazardPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_poisoned.update(
        registry, lifecycle, simulationState.m_rules.logicFramesPerSecond,
        confirmedTick, damage);
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
}

void ObjectSimulation::finishPoisonHazardPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    object_simulation_detail::state(*this).m_poisoned.finishUpdate(
        registry, lifecycle, confirmedTick);
}

void ObjectSimulation::updateOverchargeHazardPhase(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_overcharge.update(
        registry, lifecycle, simulationState.m_rules, confirmedTick, damage);
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
}

} // namespace engine
