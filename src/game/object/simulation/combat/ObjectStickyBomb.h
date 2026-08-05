#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>

#include "game/object/plan/combat/ObjectStickyBombPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class PlayerRegistry;
struct ObjectSimulationRules;

namespace sticky_bomb {

enum class DetonationTrigger : uint8_t {
    Remote,
    Timed,
    BoobyTrap,
    Script,
};

enum class PresentationKind : uint8_t {
    CreatedAudio,
    PingAudio,
    GeometryDamageFx,
};

} // namespace sticky_bomb

struct ObjectStickyBombAttachRequest final {
    ObjectId bomb = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    ObjectId bomber = INVALID_OBJECT_ID;
    std::optional<LogicFixedVec3> specificPosition;
    uint64_t confirmedTick = 0;
};

struct ObjectStickyBombPresentationEvent final {
    sticky_bomb::PresentationKind kind =
        sticky_bomb::PresentationKind::PingAudio;
    ObjectId bomb = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    container::String resource;
    math::q32_32 overrideRadius{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectStickyBombRuntime final {
    ObjectId target = INVALID_OBJECT_ID;
    ObjectId bomber = INVALID_OBJECT_ID;
    LogicFixedVec3 fixedGroundPosition{};
    std::optional<uint64_t> dieTick;
    uint64_t nextPingTick = 0;
    bool attached = false;
    bool fixedToGround = false;
    bool detonated = false;
};

struct ObjectStickyBombComponent final {
    container::SharedPtr<const game::ObjectStickyBombPlan> plan;
    container::Vector<ObjectStickyBombRuntime> instances;
    bool boobyTrap = false;
};

struct ObjectStickyBombState final {
    ObjectId bomb = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    ObjectId bomber = INVALID_OBJECT_ID;
    std::optional<uint64_t> dieTick;
    uint64_t nextPingTick = 0;
    bool timed = false;
    bool attached = false;
    bool fixedToGround = false;
    bool detonated = false;
};

class ObjectStickyBombSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    [[nodiscard]] bool attach(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain,
        const ObjectStickyBombAttachRequest& request,
        std::optional<uint64_t> lifetimeDueTick,
        uint32_t logicFramesPerSecond,
        container::Vector<ObjectStickyBombPresentationEvent>& events) const;

    [[nodiscard]] bool retarget(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId bomb, ObjectId target) const;

    [[nodiscard]] bool detonate(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, ObjectId bomb,
        sticky_bomb::DetonationTrigger trigger, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& damage,
        container::Vector<ObjectStickyBombPresentationEvent>& events) const;

    [[nodiscard]] bool detonateHostileBoobyTrapOnTarget(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, const GameContentSnapshot& content,
        ObjectId source, ObjectId target, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& damage,
        container::Vector<ObjectStickyBombPresentationEvent>& events) const;

    // Object::onDie preamble. A dying host detonates its attached booby trap
    // without the ordinary hostile-user gate because there is no interacting
    // victim at this point.
    [[nodiscard]] bool detonateBoobyTrapsOnDyingTarget(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, ObjectId target,
        uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& damage,
        container::Vector<ObjectStickyBombPresentationEvent>& events) const;

    void update(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        container::Vector<ObjectStickyBombPresentationEvent>& events) const;

    [[nodiscard]] std::optional<ObjectStickyBombState> state(
        const ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId bomb) const noexcept;
};

} // namespace engine
