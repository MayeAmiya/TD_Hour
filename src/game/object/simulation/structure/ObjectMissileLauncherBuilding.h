#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

#include "game/object/plan/structure/ObjectMissileLauncherBuildingPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;

enum class ObjectMissileLauncherDoorPhase : uint8_t {
    Closed,
    Opening,
    Open,
    WaitingToClose,
    Closing,
};

struct ObjectMissileLauncherBuildingRuntime final {
    SpecialPowerContentId specialPower = INVALID_SPECIAL_POWER_CONTENT_ID;
    uint64_t doorOpenTicks = 0;
    uint64_t doorWaitOpenTicks = 0;
    uint64_t doorCloseTicks = 0;
    uint64_t timeoutTick = 0;
    uint64_t stateEnteredTick = 0;
    uint64_t visibleDurationTicks = 0;
    uint64_t activationRequestTick = 0;
    ObjectMissileLauncherDoorPhase phase =
        ObjectMissileLauncherDoorPhase::Closed;
    ObjectMissileLauncherDoorPhase timeoutPhase =
        ObjectMissileLauncherDoorPhase::Closed;
    bool activationRequested = false;
    bool openIdleAudioActive = false;
};

struct ObjectMissileLauncherBuildingComponent final {
    container::SharedPtr<const game::ObjectMissileLauncherBuildingPlan> plan;
    container::Vector<ObjectMissileLauncherBuildingRuntime> instances;
};

struct ObjectMissileLauncherFxEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectMissileLauncherDoorPhase phase =
        ObjectMissileLauncherDoorPhase::Closed;
    container::String fxList;
    LogicFixedVec3 position;
    uint64_t animationDurationTicks = 0;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

// SpecialPower admission owns recharge. It publishes only this stable
// activation fact; the sparse door state machine consumes it later in the
// same confirmed object-update boundary.
void notifyMissileLauncherSpecialPowerActivated(
    ecs::registry& registry, ecs::entity entity,
    SpecialPowerContentId specialPower, uint64_t confirmedTick) noexcept;

// A normally ready SpecialPower may be admitted in the same confirmed tick in
// which this sparse update reaches DoorOpen.  RefCode updates the door first
// and its MissileLauncherBuildingUpdate entry point requires DOOR_OPEN before
// accepting that launch.  Return true only for that matching, still-opening
// capability so the command consumer can retain the order for the next tick;
// scripted early-fire semantics (before the power is ready) remain unchanged.
[[nodiscard]] bool missileLauncherActivationMustWaitForOpenDoor(
    const ecs::registry& registry, ecs::entity entity,
    SpecialPowerContentId specialPower) noexcept;

class ObjectMissileLauncherBuildingSystem final {
public:
    void initializeObject(
        ecs::registry& registry, ecs::entity entity,
        const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
        uint64_t confirmedTick) const;

    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
        uint64_t confirmedTick, uint64_t& nextFxEmissionSequence,
        container::Vector<ObjectMissileLauncherFxEvent>& outFx,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;

    void onObjectReclaim(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;
};

} // namespace engine
