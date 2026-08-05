#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/containment/ObjectSpawnSlaveDetail.h"
#include "core/container/string_utils.h"

#include "game/base/DamageTypes.h"
#include "game/base/SimulationRandom.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectAIOpportunityTargetPolicy.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>


namespace engine::object_spawn_slave_detail {

using Fixed = math::q32_32;

[[nodiscard]] uint64_t ticks(uint32_t milliseconds,
                             uint32_t fps) noexcept {
    if (!milliseconds) return 0;
    return (static_cast<uint64_t>(milliseconds) * std::max(1u, fps) + 999u) /
        1000u;
}

[[nodiscard]] uint64_t legacyFramesAtSessionRate(
    uint32_t legacyFrames, uint32_t fps) noexcept {
    if (legacyFrames == 0) return 0;
    return (static_cast<uint64_t>(legacyFrames) * std::max(1u, fps) +
            29u) / 30u;
}

constexpr auto asciiEqual = container::asciiEqualIgnoreCase;

[[nodiscard]] Fixed distanceSquared(const LogicFixedVec3& a,
                                    const LogicFixedVec3& b) noexcept {
    const Fixed dx = a.x - b.x;
    const Fixed dy = a.y - b.y;
    const Fixed dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] Fixed distanceSquared2D(const LogicFixedVec3& a,
                                      const LogicFixedVec3& b) noexcept {
    const Fixed dx = a.x - b.x;
    const Fixed dy = a.y - b.y;
    return dx * dx + dy * dy;
}

[[nodiscard]] bool hasAnyKind(const ObjectKindOfComponent* kinds,
                              const game::ObjectKindOfMask& wanted) noexcept {
    if (wanted.none()) return true;
    return kinds && kinds->mask.test_for_any(wanted);
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf wanted) noexcept {
    return kinds && game::objectHasKind(kinds->mask, wanted);
}

[[nodiscard]] bool alive(const ecs::registry& registry, ecs::entity entity) {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return !health || !health->effectivelyDead;
}

[[nodiscard]] LogicFixedVec3 transformLocalPointFixed(
    const LogicFixedVec3& position, math::q32_32 yaw,
    math::q32_32 localX, math::q32_32 localY,
    math::q32_32 localZ) noexcept {
    const math::q32_32 cosine = math::fixed_cos(yaw);
    const math::q32_32 sine = math::fixed_sin(yaw);
    return {
        position.x +
            localX * cosine - localY * sine,
        position.y +
            localX * sine + localY * cosine,
        position.z + localZ,
    };
}

} // namespace engine::object_spawn_slave_detail
