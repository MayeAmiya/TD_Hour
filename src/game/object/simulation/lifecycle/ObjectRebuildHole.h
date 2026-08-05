#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/lifecycle/ObjectRebuildHolePlanTypes.h"
namespace engine {

class ObjectLifecycle;
struct ObjectDamageRequest;
struct ObjectSimulationRules;

enum class ObjectRebuildHolePhase : uint8_t {
    Dormant,
    WaitingForWorker,
    AwaitingWorkerSpawn,
    Reconstructing,
};

struct ObjectRebuildHoleRuntime final {
    uint64_t workerDueTick = 0;
    uint64_t revision = 0;
    ObjectId spawner = INVALID_OBJECT_ID;
    ObjectId worker = INVALID_OBJECT_ID;
    ObjectId reconstruction = INVALID_OBJECT_ID;
    container::String rebuildTemplate;
    ObjectRebuildHolePhase phase = ObjectRebuildHolePhase::Dormant;
};

struct ObjectRebuildHoleComponent final {
    container::SharedPtr<const game::ObjectRebuildHolePlan> plan;
    container::Vector<ObjectRebuildHoleRuntime> runtimes;
};

struct ObjectRebuildExposeConsumedComponent final {
    // One object can legally author more than one RebuildHoleExposeDie.
    // Consume callbacks by author slot rather than suppressing the entire
    // object after the first matching module.
    container::Vector<uint32_t> authoredOrders;
};

struct ObjectRebuildHoleExposeIntent final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId damageSource = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
    ObjectFixedTransformComponent transform;
    ObjectGeometryShape geometryShape = ObjectGeometryShape::Sphere;
    bool geometryIsSmall = true;
    math::q32_32 geometryMajorRadius{int32_t{1}};
    math::q32_32 geometryMinorRadius{int32_t{1}};
    math::q32_32 geometryHeight{int32_t{1}};
    math::q32_32 geometryBoundingCircleRadius{int32_t{1}};
    math::q32_32 geometryBoundingSphereRadius{int32_t{1}};
    container::String holeTemplate;
    container::String rebuildTemplate;
    math::q32_32 holeMaximumHealth{};
    uint32_t authoredOrder = 0;
    uint64_t submissionOrdinal = 0;
    bool transferAttackers = true;
    uint64_t confirmedTick = 0;
};

struct ObjectRebuildWorkerSpawnIntent final {
    ObjectId hole = INVALID_OBJECT_ID;
    ObjectId reconstruction = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    ObjectTeamId team = INVALID_OBJECT_TEAM_ID;
    ObjectFixedTransformComponent transform;
    container::String workerTemplate;
    container::String rebuildTemplate;
    uint32_t authoredOrder = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectRebuildCompletionIntent final {
    ObjectId hole = INVALID_OBJECT_ID;
    ObjectId worker = INVALID_OBJECT_ID;
    ObjectId reconstruction = INVALID_OBJECT_ID;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectRebuildTargetRemapIntent final {
    ObjectId from = INVALID_OBJECT_ID;
    ObjectId to = INVALID_OBJECT_ID;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

class ObjectRebuildHoleSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    void onBehaviorDie(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId object, uint32_t authoredOrder,
        uint64_t confirmedTick) const;

    void onExposeDie(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId object, ObjectId damageSource, uint32_t authoredOrder,
        uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
        container::Vector<ObjectRebuildHoleExposeIntent>& outExpose) const;

    void onDeathPostamble(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId object, const ObjectSimulationRules& rules,
        uint64_t confirmedTick, uint64_t& nextGameplaySubmissionOrdinal,
        container::Vector<ObjectRebuildTargetRemapIntent>& outRemaps) const;

    void update(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outHealing,
        container::Vector<ObjectRebuildWorkerSpawnIntent>& outWorkers,
        container::Vector<ObjectRebuildCompletionIntent>& outCompletions,
        container::Vector<ObjectRebuildTargetRemapIntent>& outRemaps) const;

    [[nodiscard]] bool startHole(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ObjectId hole, container::StringView rebuildTemplate,
        ObjectId spawner, const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool acknowledgeWorker(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hole, ObjectId worker, ObjectId reconstruction,
        uint64_t confirmedTick) const;

    [[nodiscard]] bool rejectWorker(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId hole, const ObjectSimulationRules& rules,
        uint64_t confirmedTick) const;
};

} // namespace engine
