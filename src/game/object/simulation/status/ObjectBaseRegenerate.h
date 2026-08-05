#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <limits>
#include "game/object/plan/status/ObjectBaseRegeneratePlanTypes.h"
namespace engine {

struct ObjectDamageRequest;
struct ObjectSimulationRules;
class ObjectLifecycle;

// Per-entity sleep state replaces BaseRegenerateUpdate's mutable wake frame.
// All values are confirmed ticks, so neither wall time nor renderer cadence
// can change a building's recovery schedule.
struct ObjectBaseRegenerateRuntime final {
    static constexpr uint64_t NeverWakeTick = std::numeric_limits<uint64_t>::max();

    uint64_t nextWakeTick = NeverWakeTick;
    uint64_t lastUpdateTick = NeverWakeTick;
};

struct ObjectBaseRegenerateComponent final {
    container::SharedPtr<const game::ObjectBaseRegeneratePlan> plan;
    container::Vector<ObjectBaseRegenerateRuntime> instances;
};

class ObjectBaseRegenerateSystem final {
public:
    // Called from the single object spawn assembly after the immutable
    // Archetype is attached. Positive GameData regeneration begins awake,
    // exactly like the source module constructor; a full body sleeps when it
    // receives its first confirmed update.
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    // RefCode's DamageModule callback runs only after ordinary HP actually
    // decreased. ObjectSimulation supplies that fixed transaction fact and
    // wakes every BaseRegenerateUpdate instance after the global delay.
    void onHealthDecreased(ecs::registry& registry, ObjectLifecycle& lifecycle,
                           ObjectId object, uint64_t confirmedTick,
                           const ObjectSimulationRules& rules) const;

    // Emits HEALING value requests in stable ObjectId then authored rule
    // order. ObjectSimulation's Body transaction remains the sole writer of
    // ObjectHealthComponent, including clipping to maximum health.
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const ObjectSimulationRules& rules, uint64_t confirmedTick,
                container::Vector<ObjectDamageRequest>& outDamage) const;

private:
    [[nodiscard]] static uint64_t millisecondsToTicks(uint32_t milliseconds,
                                                       uint32_t framesPerSecond) noexcept;
};

} // namespace engine
