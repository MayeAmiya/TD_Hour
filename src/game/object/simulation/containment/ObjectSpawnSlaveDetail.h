#pragma once

#include "game/object/simulation/containment/ObjectSpawnSlave.h"

#include <utility>

namespace engine::object_spawn_slave_detail {

using Fixed = math::q32_32;

struct Candidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity{};
};

struct UpdateContext final {
    ecs::registry& registry;
    ObjectLifecycle& lifecycle;
    const PlayerRegistry* players;
    const GameContentSnapshot* content;
    const ObjectSpatialIndex* spatialIndex;
    const game::terrain::TerrainLogic* terrain;
    SimulationRandom* random;
    const ObjectSimulationRules& rules;
    uint64_t confirmedTick;
    uint64_t& nextGameplaySubmissionOrdinal;
    container::Vector<ObjectSpawnSlaveRequest>& spawnRequests;
    container::Vector<ObjectDamageRequest>& damageRequests;
    container::Vector<ObjectSpawnVeterancyRequest>& veterancyRequests;
    container::Vector<ObjectBodyHealthProjection>& bodyHealthProjections;
    container::Vector<ObjectDeleteDestroyRequest>& destroyRequests;
    container::Vector<ObjectDefectionRequest>& defectionRequests;
    container::Vector<ObjectSlaveRepairPresentationEvent>&
        repairPresentationEvents;
    container::Vector<ObjectTensileFormationEvent>&
        tensileNavigationEvents;
    container::Vector<ObjectTensileFormationEvent>&
        tensilePresentationEvents;
    // Per-update typed scratch. Tensile consumes each buffer completely
    // before the next candidate/rule clears it.
    container::Vector<ObjectId>& spatialQueryScratch;
    container::Vector<std::pair<Fixed, ObjectId>>& tensileNearestScratch;
};

[[nodiscard]] inline uint64_t reserveGameplaySubmissionOrdinal(
    UpdateContext& context) noexcept {
    const uint64_t result = context.nextGameplaySubmissionOrdinal;
    ++context.nextGameplaySubmissionOrdinal;
    if (context.nextGameplaySubmissionOrdinal == 0)
        ++context.nextGameplaySubmissionOrdinal;
    return result;
}

[[nodiscard]] uint64_t ticks(uint32_t milliseconds, uint32_t fps) noexcept;
[[nodiscard]] uint64_t legacyFramesAtSessionRate(
    uint32_t legacyFrames, uint32_t fps) noexcept;
[[nodiscard]] Fixed distanceSquared(
    const LogicFixedVec3& a, const LogicFixedVec3& b) noexcept;
[[nodiscard]] Fixed distanceSquared2D(
    const LogicFixedVec3& a, const LogicFixedVec3& b) noexcept;
[[nodiscard]] bool hasAnyKind(
    const ObjectKindOfComponent* kinds,
    const game::ObjectKindOfMask& wanted) noexcept;
[[nodiscard]] bool hasKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf wanted) noexcept;
[[nodiscard]] bool alive(const ecs::registry& registry, ecs::entity entity);
[[nodiscard]] LogicFixedVec3 transformLocalPointFixed(
    const LogicFixedVec3& position, math::q32_32 yaw,
    math::q32_32 localX, math::q32_32 localY,
    math::q32_32 localZ) noexcept;

void reconcileOwnership(UpdateContext& context);
void updateSpawnAndHordeCandidate(
    UpdateContext& context, const container::Vector<Candidate>& objects,
    const Candidate& candidate);
void updateTensileCandidate(
    UpdateContext& context, const container::Vector<Candidate>& objects,
    const Candidate& candidate);
void updateSlavedCandidate(UpdateContext& context, const Candidate& candidate);

} // namespace engine::object_spawn_slave_detail
