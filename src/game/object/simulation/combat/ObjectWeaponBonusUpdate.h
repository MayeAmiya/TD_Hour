#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/definition/ObjectKindOf.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>
#include "game/object/plan/combat/ObjectWeaponBonusUpdatePlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class PlayerRegistry;
class SimulationRandom;

struct ObjectWeaponBonusUpdateRuntime final {
    uint64_t nextPulseTick = 0;
    uint64_t lastPulseTick = UINT64_MAX;
};

struct ObjectWeaponBonusUpdateComponent final {
    container::SharedPtr<const game::ObjectWeaponBonusUpdatePlan> plan;
    container::Vector<ObjectWeaponBonusUpdateRuntime> instances;
};

// RefCode has one TempWeaponBonusHelper per Object. Different temporary
// statuses replace each other; reapplying the same status refreshes expiry.
struct ObjectTemporaryWeaponBonusComponent final {
    std::optional<game::WeaponBonusCondition> current;
    uint64_t removeTick = 0;
    // Drawable's FRENZY tint is a 30-frame attack/release envelope. Keep the
    // presentation clock detached from the bonus bit so expiry can fade out
    // without extending gameplay state.
    uint64_t tintStartedTick = 0;
    uint64_t tintReleaseTick = 0;
    // 1..30 表示 release 起点的离散 attack 包络；0 表示无 release。
    uint8_t tintReleaseStartFrame = 0;
};

enum class ObjectWeaponBonusUpdateEventKind : uint8_t {
    Applied,
    Cleared,
};

struct ObjectWeaponBonusUpdateEvent final {
    ObjectWeaponBonusUpdateEventKind kind =
        ObjectWeaponBonusUpdateEventKind::Applied;
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    game::WeaponBonusCondition condition = game::WeaponBonusCondition::Count;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

class ObjectWeaponBonusUpdateSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          uint64_t createdAtTick) const;

    // Expiry runs before pulses. Thus a zero-duration pulse is active for the
    // combat phase in which it was emitted and is removed at the next system
    // boundary, while an equal Delay/Duration refresh remains deterministic.
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const PlayerRegistry& players,
                const GameContentSnapshot& content,
                SimulationRandom& random,
                uint32_t logicFramesPerSecond,
                uint64_t confirmedTick,
                container::Vector<ObjectWeaponBonusUpdateEvent>& outEvents) const;

private:
    [[nodiscard]] static uint64_t millisecondsToTicks(
        uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
};

} // namespace engine
