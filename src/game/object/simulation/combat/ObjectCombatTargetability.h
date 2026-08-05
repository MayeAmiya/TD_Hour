#pragma once

#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"

namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;

// Read-only equivalent of the two RefCode acquisition filters used by
// AI::findClosestEnemy: PartitionFilterPossibleToAttack and
// PartitionFilterWithinAttackRange.  Keeping both facts separate preserves
// the original rule that any authored weapon may satisfy the range filter
// while another compatible weapon satisfies targetability.
struct ObjectCombatTargetability final {
    bool canAttack = false;
    bool withinAnyWeaponRange = false;
};

// Read-only own-weapon branch of RefCode Object::isAbleToAttack().  This is
// deliberately narrower than the complete legacy predicate: weaponless
// transports, SpawnBehavior masters and EnterGuard objects are handled by
// their dedicated command/state owners.  Callers here need to know whether
// this object's currently selected weapon set may participate in an attack.
[[nodiscard]] bool objectOwnWeaponsAbleToAttack(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ecs::entity source,
    uint64_t confirmedTick) noexcept;

[[nodiscard]] ObjectCombatTargetability queryObjectCombatTargetability(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId source,
    ObjectId target, uint64_t confirmedTick) noexcept;

} // namespace engine
