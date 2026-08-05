#pragma once

#include "game/object/simulation/combat/ObjectProjectileSystem.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"

#include <limits>

namespace engine::object_projectile_detail {

using Fixed = ObjectProjectileComponent::Scalar;
using FixedVec3 = LogicFixedVec3;

constexpr uint32_t kMaximumPathSegments = static_cast<uint32_t>(
    std::numeric_limits<int32_t>::max());

inline const Fixed kFixedZero{int32_t{0}};
inline const Fixed kFixedOne{int32_t{1}};
inline const Fixed kFixedTwo{int32_t{2}};
inline const Fixed kFixedThree{int32_t{3}};
inline const Fixed kFixedFour{int32_t{4}};
inline const Fixed kFixedHalf = Fixed::from_fraction(1, 2);
inline const Fixed kFixedSegmentEpsilon = Fixed::from_fraction(1, 100000);
inline const Fixed kFixedTargetAdjustThresholdSquared =
    Fixed::from_fraction(1, 10);

enum class MissileWaypointAdvance : uint8_t {
    Advanced,
    Terminal,
    Invalid,
};

struct ProjectileCollision final {
    Fixed time{};
    ObjectId target = INVALID_OBJECT_ID;
};

[[nodiscard]] FixedVec3 missileWaypointTarget(
    const game::terrain::WaypointRecord& waypoint) noexcept;
[[nodiscard]] MissileWaypointAdvance advanceMissileWaypoint(
    const game::terrain::TerrainLogic& terrain, ObjectId projectileId,
    const ObjectProjectileComponent& projectile,
    ObjectProjectileWaypointPathComponent& route,
    FixedVec3& nextTarget) noexcept;

[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
[[nodiscard]] math::vec3 presentationPosition(
    const FixedVec3& value) noexcept;
[[nodiscard]] uint32_t snapshotPathfindLayer(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint32_t fallback) noexcept;

[[nodiscard]] FixedVec3 add(
    const FixedVec3& left, const FixedVec3& right) noexcept;
[[nodiscard]] FixedVec3 subtract(
    const FixedVec3& left, const FixedVec3& right) noexcept;
[[nodiscard]] FixedVec3 scale(
    const FixedVec3& value, Fixed amount) noexcept;
[[nodiscard]] Fixed dot(
    const FixedVec3& left, const FixedVec3& right) noexcept;
[[nodiscard]] Fixed squaredLength(const FixedVec3& value) noexcept;
[[nodiscard]] Fixed length(const FixedVec3& value) noexcept;
[[nodiscard]] Fixed planarLength(const FixedVec3& value) noexcept;
[[nodiscard]] Fixed minFixed(Fixed left, Fixed right) noexcept;
[[nodiscard]] Fixed maxFixed(Fixed left, Fixed right) noexcept;
[[nodiscard]] Fixed clampUnit(Fixed value) noexcept;
[[nodiscard]] FixedVec3 normalizedOr(
    const FixedVec3& value, const FixedVec3& fallback) noexcept;
[[nodiscard]] FixedVec3 quaternionForward(
    const LogicFixedQuaternion& rotation) noexcept;
[[nodiscard]] FixedVec3 quaternionLocalY(
    const LogicFixedQuaternion& rotation) noexcept;
[[nodiscard]] FixedVec3 quaternionLocalZ(
    const LogicFixedQuaternion& rotation) noexcept;
[[nodiscard]] Fixed clampFixed(
    Fixed value, Fixed minimum, Fixed maximum) noexcept;
[[nodiscard]] FixedVec3 deterministicPerpendicular(
    const FixedVec3& forward) noexcept;
[[nodiscard]] FixedVec3 turnToward(
    const FixedVec3& current, const FixedVec3& desired,
    Fixed maximumRadians) noexcept;
[[nodiscard]] Fixed deterministicSignedUnit(
    ObjectId object, uint64_t tick, uint64_t lane) noexcept;
[[nodiscard]] uint32_t ceilPositiveRatio(
    Fixed numerator, Fixed denominator) noexcept;
[[nodiscard]] Fixed fraction(
    uint32_t numerator, uint32_t denominator) noexcept;
[[nodiscard]] FixedVec3 cubicPointAtStep(
    const ObjectProjectileComponent& projectile, uint32_t step) noexcept;
void refreshFlightPathForward(
    ObjectProjectileComponent& projectile, uint32_t step) noexcept;
void projectFlightPathYaw(
    ecs::registry& registry, ecs::entity entity,
    TransformComponent& transform,
    const ObjectProjectileComponent& projectile) noexcept;
[[nodiscard]] bool rebuildDumbControls(
    ObjectProjectileComponent& projectile,
    const game::terrain::TerrainLogic& terrain) noexcept;
[[nodiscard]] Fixed approximateBezierLength(
    const ObjectProjectileComponent& projectile) noexcept;

[[nodiscard]] std::optional<ProjectileCollision> findProjectileCollision(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex* spatialIndex, const PlayerRegistry* players,
    ecs::entity projectileEntity, ObjectId projectileId,
    const ObjectProjectileComponent& projectile,
    const FixedVec3& start, const FixedVec3& destination,
    Fixed projectileRadius, const game::WeaponTemplate* weapon,
    bool useWeaponFilter,
    container::Span<const ObjectId> currentProjectileIds,
    container::Vector<ObjectId>& candidateScratch,
    uint64_t confirmedTick);
[[nodiscard]] bool clearGarrisonOnImpact(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId projectileId, ecs::entity projectileEntity,
    ObjectProjectileComponent& projectile, ObjectId targetId,
    const FixedVec3& impact, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectProjectileEvent>& events);
[[nodiscard]] std::optional<FixedVec3> bridgeLayerImpact(
    ObjectProjectileComponent& projectile,
    const game::terrain::TerrainLogic& terrain,
    const FixedVec3& destination) noexcept;
void synchronizeProjectileTerrainLayer(
    ecs::registry& registry, ecs::entity entity,
    const ObjectProjectileComponent& projectile,
    uint64_t confirmedTick);
void detonate(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content,
    const ObjectSpatialIndex* spatialIndex,
    const PlayerRegistry* players, ObjectId projectileId,
    ecs::entity projectileEntity,
    ObjectProjectileComponent& projectile,
    TransformComponent& transform, const FixedVec3& position,
    ObjectProjectileEventKind kind, ObjectId collidedTarget,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectId>& damageVictimScratch,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectHistoricBonusWeaponFire>& outHistoricBonusWeapons,
    container::Vector<ObjectProjectileEvent>& events,
    bool applyDetonationWeapon = true,
    bool deferDestruction = false);

void publishGuidedProjectileTransform(
    ecs::registry& registry, ecs::entity entity,
    ObjectProjectileComponent& projectile, TransformComponent& transform,
    const FixedVec3& position, const FixedVec3& forward) noexcept;
[[nodiscard]] bool updateMissileProjectile(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content,
    const ObjectSpatialIndex* spatialIndex,
    const PlayerRegistry* players,
    const game::terrain::TerrainLogic& terrain,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    ObjectId projectileId, ecs::entity entity,
    ObjectProjectileComponent& projectile,
    ObjectMissileProjectileComponent& missile,
    TransformComponent& transform,
    container::Vector<ObjectId>& collisionCandidateScratch,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectHistoricBonusWeaponFire>& outHistoricBonusWeapons,
    container::Vector<ObjectProjectileEvent>& events);
[[nodiscard]] bool updateNeutronProjectile(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content,
    const ObjectSpatialIndex* spatialIndex,
    const PlayerRegistry* players,
    const game::terrain::TerrainLogic& terrain,
    uint64_t confirmedTick, ObjectId projectileId, ecs::entity entity,
    ObjectProjectileComponent& projectile,
    ObjectNeutronMissileProjectileComponent& neutron,
    TransformComponent& transform,
    container::Vector<ObjectId>& collisionCandidateScratch,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectProjectileEvent>& events);

} // namespace engine::object_projectile_detail
