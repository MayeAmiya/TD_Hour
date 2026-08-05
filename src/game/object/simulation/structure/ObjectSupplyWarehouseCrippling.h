#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

#include "game/object/plan/structure/ObjectSupplyWarehouseCripplingPlanTypes.h"
namespace engine {

struct ObjectDamageRequest;
struct ObjectHealthEvent;
struct ObjectSimulationRules;
class ObjectLifecycle;

// Dock implementations consume one compact reason mask instead of retaining
// a pointer to the legacy SupplyWarehouseCripplingBehavior.  Keeping the
// reason typed lets later sabotage/disabled dock producers coexist without
// one subsystem accidentally reopening a dock another subsystem closed.
enum class ObjectDockCrippleReason : uint8_t {
    SupplyWarehouseReallyDamaged,
    Count,
};

using ObjectDockCrippleReasonMask = uint8_t;

[[nodiscard]] constexpr ObjectDockCrippleReasonMask
objectDockCrippleReasonBit(ObjectDockCrippleReason reason) noexcept {
    static_assert(static_cast<uint8_t>(ObjectDockCrippleReason::Count) < 8);
    return reason >= ObjectDockCrippleReason::Count
        ? ObjectDockCrippleReasonMask{}
        : static_cast<ObjectDockCrippleReasonMask>(
              ObjectDockCrippleReasonMask{1} << static_cast<uint8_t>(reason));
}

struct ObjectDockCrippleComponent final {
    ObjectDockCrippleReasonMask reasons = 0;

    [[nodiscard]] constexpr bool crippled() const noexcept {
        return reasons != 0;
    }

    constexpr void set(ObjectDockCrippleReason reason, bool active) noexcept {
        const ObjectDockCrippleReasonMask bit = objectDockCrippleReasonBit(reason);
        reasons = active
            ? static_cast<ObjectDockCrippleReasonMask>(reasons | bit)
            : static_cast<ObjectDockCrippleReasonMask>(reasons & ~bit);
    }
};

// Mutable module-local state. All clocks and converted intervals are
// confirmed ticks; NeverWakeTick is the modern replacement for
// UPDATE_SLEEP_FOREVER.
struct ObjectSupplyWarehouseCripplingRuntime final {
    static constexpr uint64_t NeverWakeTick =
        std::numeric_limits<uint64_t>::max();

    uint64_t selfHealSuppressionTicks = 0;
    uint64_t selfHealDelayTicks = 0;
    uint64_t nextHealingTick = NeverWakeTick;
};

struct ObjectSupplyWarehouseCripplingComponent final {
    container::SharedPtr<const game::ObjectSupplyWarehouseCripplingPlan> plan;
    container::Vector<ObjectSupplyWarehouseCripplingRuntime> instances;
};

class ObjectSupplyWarehouseCripplingSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const ObjectSimulationRules& rules) const;

    // Receives committed Body facts. Only an actual HP decrease resets the
    // heal suppression clock; BodyDamageState transitions independently
    // maintain the reusable dock-crippled fact.
    void onHealthEvent(ecs::registry& registry, ObjectLifecycle& lifecycle,
                       const ObjectHealthEvent& event) const;

    // Emits fixed-point HEALING requests in stable ObjectId/authored order.
    // ObjectSimulation remains the sole ObjectHealthComponent writer.
    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                uint64_t confirmedTick,
                container::Vector<ObjectDamageRequest>& outDamage) const;

private:
    [[nodiscard]] static uint64_t millisecondsToTicks(
        uint32_t milliseconds, uint32_t framesPerSecond) noexcept;
};

} // namespace engine
