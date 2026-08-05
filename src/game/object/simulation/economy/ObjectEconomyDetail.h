#pragma once

#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"

namespace engine::object_economy_detail {

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept;

[[nodiscard]] uint64_t saturatingAdd(
    uint64_t left, uint64_t right) noexcept;

[[nodiscard]] math::q32_32 distanceSquared2D(
    const LogicFixedVec3& left,
    const LogicFixedVec3& right) noexcept;

[[nodiscard]] bool hasKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept;

[[nodiscard]] bool isAliveObject(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object) noexcept;

[[nodiscard]] bool hasBlockingStatus(
    const ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick) noexcept;

[[nodiscard]] math::q32_32 dockingDistanceSquaredLimit(
    const ObjectGeometryComponent* docker,
    const ObjectGeometryComponent* dock) noexcept;

[[nodiscard]] bool hasEconomyModule(
    const ObjectEconomyComponent* economy,
    game::ObjectEconomyModuleKind kind) noexcept;

} // namespace engine::object_economy_detail
