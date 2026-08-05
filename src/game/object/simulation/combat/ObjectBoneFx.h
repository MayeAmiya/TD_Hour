#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"

#include "game/base/DamageTypes.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/simulation/combat/ObjectTransitionDamageFx.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "game/object/plan/combat/ObjectBoneFxPlanTypes.h"
namespace engine {

struct ObjectHealthEvent;
struct ObjectSimulationRules;
class ObjectLifecycle;
class GameContentSnapshot;

struct ObjectBoneFxRuntimeEntry final {
    uint64_t dueTick = std::numeric_limits<uint64_t>::max();
    uint32_t sampleOrdinal = 0;
};

struct ObjectBoneFxRuntimeRule final {
    container::Vector<ObjectBoneFxRuntimeEntry> entries;
    ObjectBodyDamageState state = ObjectBodyDamageState::Pristine;
    uint32_t activationEpoch = 0;
    bool active = false;
};

struct ObjectBoneFxComponent final {
    container::SharedPtr<const game::ObjectBoneFxPlan> plan;
    container::Vector<ObjectBoneFxRuntimeRule> rules;
    // Minimum due tick across every active authored entry. Zero means the
    // rules still need their first state initialization; max means no entry
    // can wake again. The outer update can reject dormant objects before
    // building and sorting its stable ObjectId work list.
    uint64_t nextDueTick = 0;
    // stopAllBoneFX() clears every current timer and running particle system.
    // ZH does not make that stop permanent: a later body-damage transition
    // calls changeBodyDamageState()/initTimes() and re-arms the new state.
    bool stopped = false;
};

struct ObjectBoneFxStopRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t callerAuthoredOrder = 0;
    uint64_t confirmedTick = 0;
};

class ObjectBoneFxSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    // BoneFXDamage is the state-change bridge for BoneFXUpdate. This executes
    // at ActiveBody's onBodyDamageStateChange boundary, kills running particle
    // groups and re-arms the target state's periodic entries.
    void onHealthEvent(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectHealthEvent& event, const ObjectSimulationRules& rules,
        uint64_t sessionSeed, uint64_t& nextEmissionSequence,
        container::Vector<ObjectTransitionDamageFxEvent>& output) const;

    [[nodiscard]] bool stopAll(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectBoneFxStopRequest& request,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectTransitionDamageFxEvent>& output) const;

    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot* content,
        const ObjectSimulationRules& rules, uint64_t sessionSeed,
        uint64_t confirmedTick, uint64_t& nextEmissionSequence,
        container::Vector<ObjectTransitionDamageFxEvent>& output) const;
};

} // namespace engine
