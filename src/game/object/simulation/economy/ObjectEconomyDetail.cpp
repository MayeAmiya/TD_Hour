#include "game/object/simulation/economy/ObjectEconomy.h"

#include "game/object/simulation/economy/ObjectEconomyDetail.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/runtime/ObjectHackInternetOrderAdapter.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/base/GameBalanceConstants.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

namespace engine::object_economy_detail {

namespace {

constexpr auto equalInsensitive = container::asciiEqualIgnoreCase;

} // namespace

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * fps + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return left > std::numeric_limits<uint64_t>::max() - right
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
}

[[nodiscard]] math::q32_32 distanceSquared2D(
    const engine::LogicFixedVec3& left,
    const engine::LogicFixedVec3& right) noexcept {
    const math::q32_32 dx = left.x - right.x;
    const math::q32_32 dy = left.y - right.y;
    return dx * dx + dy * dy;
}

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool isAliveObject(const ecs::registry& registry,
                                 const engine::ObjectLifecycle& lifecycle,
                                 ecs::entity entity,
                                 engine::ObjectId object) noexcept {
    if (!object || !lifecycle.entityFromId(object) ||
        lifecycle.isPendingDestroy(object)) {
        return false;
    }
    const engine::ObjectHealthComponent* health =
        ecs::try_get<engine::ObjectHealthComponent>(registry, entity);
    // acceptsDamage is a damage-routing capability, not a lifecycle bit.
    // Non-damageable supply docks, workers and economy-only fixtures remain
    // valid owner participants until they are explicitly dead/destroyed.
    return !health || !health->acceptsDamage || !health->effectivelyDead;
}

[[nodiscard]] bool hasBlockingStatus(const ecs::registry& registry,
                                     ecs::entity entity,
                                     uint64_t confirmedTick) noexcept {
    const engine::ObjectStatusComponent* status =
        ecs::try_get<engine::ObjectStatusComponent>(registry, entity);
    return (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Destroyed) |
            game::objectStatusBit(game::ObjectStatusFlag::Sold) |
            game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction))) ||
        isObjectDisabled(registry, entity, confirmedTick);
}


[[nodiscard]] math::q32_32 dockingDistanceSquaredLimit(
    const engine::ObjectGeometryComponent* docker,
    const engine::ObjectGeometryComponent* dock) noexcept {
    const math::q32_32 dockerRadius = docker
        ? math::q32_32::max(math::q32_32{},
                            docker->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{10}};
    const math::q32_32 dockRadius = dock
        ? math::q32_32::max(math::q32_32{},
                            dock->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{10}};
    const math::q32_32 closeEnough = math::q32_32::max(
        math::q32_32{int32_t{1}}, dockerRadius + dockRadius);
    return closeEnough * closeEnough;
}

[[nodiscard]] bool hasEconomyModule(
    const engine::ObjectEconomyComponent* economy,
    game::ObjectEconomyModuleKind kind) noexcept {
    return economy && economy->plan && std::any_of(
        economy->plan->modules.begin(), economy->plan->modules.end(),
        [kind](const game::ObjectEconomyModulePresence& module) {
            return module.kind == kind;
        });
}


} // namespace engine::object_economy_detail
