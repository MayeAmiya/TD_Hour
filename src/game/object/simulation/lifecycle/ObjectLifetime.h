#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>
#include "game/object/plan/lifecycle/ObjectLifetimePlanTypes.h"
namespace engine {

struct ObjectSimulationRules;
class ObjectLifecycle;

// Every authored timer owns an independent absolute deadline. `armed` must
// remain distinct from dueTick: creation at tick zero and a zero-valued
// source duration are both legal, while the latter still wakes next frame.
struct ObjectLifetimeRuntime final {
    uint64_t dueTick = 0;
    // Each explicit reschedule gets a distinct counter-PRF sample. This
    // replaces the legacy global-RNG consumption while preserving the fact
    // that repeated setLifetimeRange() calls do not keep reusing one roll.
    uint64_t scheduleGeneration = 0;
    bool armed = false;
    bool fired = false;
};

struct ObjectLifetimeComponent final {
    container::SharedPtr<const game::ObjectLifetimePlan> plan;
    container::Vector<ObjectLifetimeRuntime> instances;
    // Frozen at spawn so a later script override cannot retroactively rewrite
    // an existing module's deadline, matching the legacy constructor check.
    bool isHulk = false;
    std::optional<uint32_t> hulkOverrideFramesAtSpawn;
};

// A value-only command produced in ObjectId then final recipe order. The
// ObjectSimulation owns execution so a Lifetime kill can synchronously enter
// the sole Body/Die writer before a later Deletion rule is considered.
struct ObjectLifetimeCommand final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    game::ObjectLifetimeAction action = game::ObjectLifetimeAction::Kill;
};

// Runtime callers such as OCL, special-power view objects, and future
// production helpers use logic frames already, exactly like the legacy
// setLifetimeRange(UnsignedInt, UnsignedInt) interface.  An omitted
// authoredOrder selects the first matching final-recipe timer; callers that
// need to address a particular timer must provide its immutable order key.
struct ObjectLifetimeRescheduleRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    game::ObjectLifetimeAction action = game::ObjectLifetimeAction::Kill;
    std::optional<uint32_t> authoredOrder;
    uint32_t minimumLifetimeFrames = 0;
    uint32_t maximumLifetimeFrames = 0;
    uint64_t confirmedTick = 0;
};

class ObjectLifetimeSystem final {
public:
    // Called by ObjectSimulation's common spawn assembly. It copies the
    // immutable Archetype plan and immediately arms every deadline from the
    // creation tick, matching legacy module construction. The counter PRF
    // needs no mutable global RNG or Session API.
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const ObjectSimulationRules& rules,
                          uint64_t sessionSeed,
                          std::optional<uint32_t> hulkLifetimeOverrideFrames) const;

    // Consumes armed due times and emits every action in canonical order.
    // It defensively arms a manually attached component from
    // ObjectLifecycleComponent::createdAtTick, but normal spawn has already
    // done so before Created is published. `sessionSeed` drives a counter PRF,
    // not a shared mutable random stream, so another object/module can never
    // perturb an already spawned timer.
    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const ObjectSimulationRules& rules, uint64_t sessionSeed,
                uint64_t confirmedTick, container::Vector<ObjectLifetimeCommand>& outCommands) const;

    // Modern value-only equivalent of LifetimeUpdate/DeletionUpdate's
    // setLifetimeRange().  It re-arms one live timer from `confirmedTick`;
    // it does not inspect INI data or expose an EnTT entity to the caller.
    [[nodiscard]] bool reschedule(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                                  const ObjectLifetimeRescheduleRequest& request,
                                  uint64_t sessionSeed) const;

    // Value-only replacement for the legacy getDieFrame(). A normal spawn
    // arms the timer before other initialization consumers run, so this query
    // never needs to advance a simulation tick or mutate an entity.
    [[nodiscard]] std::optional<uint64_t> nextDueTick(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, game::ObjectLifetimeAction action,
        std::optional<uint32_t> authoredOrder = std::nullopt) const;
};

} // namespace engine
