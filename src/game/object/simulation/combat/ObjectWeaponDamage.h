#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "core/ecs/ObjectId.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstdint>
#include <optional>
namespace engine {

class ObjectLifecycle;
class ObjectSpatialIndex;
class PlayerRegistry;

// A fully resolved WeaponTemplate impact.  It deliberately distinguishes the
// physical object that filters area damage from the ObjectId credited with
// that damage: projectile detonation uses the projectile for geometry,
// affinity and cone checks, but credits the launcher exactly like RefCode.
// Direct weapons simply use the attacker for both fields.
struct ObjectWeaponImpact final {
    ObjectId filterSource = INVALID_OBJECT_ID;
    ObjectId damageCredit = INVALID_OBJECT_ID;
    // A projectile's producer is protected by the same ordinary
    // RadiusDamageAffects=SELF rule as the physical projectile itself.
    ObjectId producer = INVALID_OBJECT_ID;
    // Coupled impact requests preserve this physical source's local order in
    // ObjectSimulation.  Projectile detonation uses its own ObjectId here;
    // ordinary direct weapon damage leaves it invalid.
    ObjectId causalGroup = INVALID_OBJECT_ID;
    ecs::entity filterSourceEntity = ecs::null;
    // A direct object attack has a primary target; position/detonation damage
    // deliberately leaves this invalid so every victim observes Affects.
    ObjectId primaryTarget = INVALID_OBJECT_ID;
    LogicFixedVec3 impactPosition{};
    // A physical projectile supplies its normalized, fixed-point trajectory
    // tangent here.  RadiusDamageAngle then uses the exact impact segment in
    // 3D instead of a stale 2D presentation yaw.  Direct weapons leave this
    // unset and continue using their actor's transform orientation.
    std::optional<LogicFixedVec3> fixedForward;
    const game::WeaponTemplate* weapon = nullptr;
    game::WeaponBonus bonus;
    uint32_t sourceSequence = 0;
    uint64_t confirmedTick = 0;
};

// Emits deterministic value-only health transactions.  It owns the original
// 3D bounding-sphere area test, RadiusDamageAffects and directional-cone
// filtering, but never mutates Health, ECS structure, audio or rendering.
void appendWeaponImpactDamage(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                              const ObjectSpatialIndex* spatialIndex,
                              const PlayerRegistry* players,
                              const ObjectWeaponImpact& impact,
                              container::Vector<ObjectDamageRequest>& outDamage);

// Hot-loop form. victimScratch is cleared on entry and remains ObjectId
// sorted/unique after broad-phase expansion so callers can retain capacity
// without changing authoritative damage ordering.
void appendWeaponImpactDamage(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex* spatialIndex, const PlayerRegistry* players,
    const ObjectWeaponImpact& impact,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectId>& victimScratch);

} // namespace engine
