#pragma once

#include "core/ecs/registry.h"

#include <cstdint>

namespace engine {

struct ObjectProductionSpawnIntent;
struct ObjectProductionExitRoute;
struct ObjectSpawnSlaveRequest;
namespace navigation { class NavigationSystem; }

namespace production_exit {

void queueProducedUnitRoute(
    ecs::registry& registry,
    ecs::entity entity,
    const ObjectProductionSpawnIntent& intent,
    uint64_t confirmedTick,
    const navigation::NavigationSystem& navigationSystem);

void queueProductionExitRoute(
    ecs::registry& registry,
    ecs::entity entity,
    const ObjectProductionExitRoute& route,
    uint64_t confirmedTick,
    const navigation::NavigationSystem& navigationSystem);

void queueSpawnSlaveRoute(
    ecs::registry& registry,
    ecs::entity entity,
    const ObjectSpawnSlaveRequest& request);

} // namespace production_exit
} // namespace engine
