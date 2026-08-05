#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/player/PlayerRegistry.h"

#include <utility>

namespace engine {

void ObjectSimulation::updatePreCombatStatusEffects(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, SimulationRandom& random,
    uint64_t confirmedTick, ObjectUpgradeExecutionContext context) {
    auto& simulationState = object_simulation_detail::state(*this);
    auto& damage = simulationState.m_damageScratch;
    damage.clear();
    simulationState.m_empUpdate.update(
        registry, lifecycle, players, random,
        simulationState.m_rules.logicFramesPerSecond, confirmedTick,
        damage, simulationState.m_empParticleEvents);
    simulationState.m_leafletDrop.update(
        registry, lifecycle, players,
        simulationState.m_rules.logicFramesPerSecond,
        confirmedTick, simulationState.m_leafletParticleEvents);
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }

    // Autonomous hazard cleanup must publish its typed Attack/Move intent
    // before Combat samples queue heads.  A per-runtime tick guard makes this
    // idempotent when the later all-object update reaches the same boundary.
    if (context.content) {
        simulationState.m_cleanupHazard.update(
            registry, lifecycle, *context.content, simulationState.m_rules, &random,
            confirmedTick);
    }
}

} // namespace engine
