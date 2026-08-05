#pragma once

#include "core/container/container_types.h"

#include <cstdint>
#include "core/ecs/registry.h"
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include "game/object/plan/economy/ObjectAutoDepositPlanTypes.h"
namespace engine {

struct ObjectSimulationRules;
class ObjectLifecycle;
class PlayerRegistry;
class UpgradeCatalog;

struct ObjectAutoDepositRuntime final {
    uint64_t nextDepositTick = 0;
    bool initialized = false;
    bool captureBonusArmed = false;
};

struct ObjectAutoDepositComponent final {
    container::SharedPtr<const game::ObjectAutoDepositPlan> plan;
    container::Vector<ObjectAutoDepositRuntime> instances;
};

enum class ObjectAutoDepositEventKind : uint8_t {
    PeriodicIncome,
    InitialCaptureBonus,
};

// Detached economy/presentation fact. It records the authored base amount
// separately because RefCode scores periodic income from DepositAmount rather
// than the upgrade-boosted total. A later ScoreKeeper/UI bridge can consume
// this without retaining Object, Player, Drawable or renderer handles.
struct ObjectAutoDepositEvent final {
    ObjectAutoDepositEventKind kind = ObjectAutoDepositEventKind::PeriodicIncome;
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId player = INVALID_PLAYER_ID;
    int64_t amount = 0;
    int32_t baseAmount = 0;
    int32_t upgradeBoost = 0;
    uint32_t authoredOrder = 0;
    bool actualMoney = true;
    bool deposited = false;
    uint64_t confirmedTick = 0;
};

class ObjectAutoDepositSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                PlayerRegistry* players, const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectAutoDepositEvent>& outEvents,
                const UpgradeCatalog* upgradeCatalog = nullptr) const;

    // Player::becomingTeamMember invokes the source hook only for a
    // non-neutral new controller. The modern session may call this for every
    // ownership transition, but neutral ownership is filtered without
    // resetting the deadline or consuming the one-shot arm.
    void onObjectOwnerChanged(ecs::registry& registry,
                              ObjectLifecycle& lifecycle,
                              ObjectId object, PlayerRegistry& players,
                              PlayerId newOwner,
                              const ObjectSimulationRules& rules,
                              uint64_t confirmedTick,
                              container::Vector<ObjectAutoDepositEvent>& outEvents) const;

private:
    [[nodiscard]] static uint64_t millisecondsToTicks(
        uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
};

} // namespace engine
