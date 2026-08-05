#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "math/fixed/q32_32.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"

#include <cstdint>

namespace engine {

class ObjectLifecycle;
enum class ObjectPanelFlag : uint8_t;
enum class ObjectAIAttitude : int8_t;
struct SpecialPowerDefinition;
namespace script {
enum class ScriptSpecialPowerCountdownOperation : uint8_t;
}

// Executes small, synchronous object-state transactions against the live ECS.
// Admission order and confirmed-tick validation belong to the calling domain
// port; this service owns object-liveness validation and the complete component
// mutation so callers never reach through the Session composition root.
class GameSessionObjectStateTransactions final {
public:
    GameSessionObjectStateTransactions(
        ecs::registry& registry, ObjectLifecycle& objects) noexcept;

    [[nodiscard]] bool setHeld(
        ObjectId object, bool held, uint64_t confirmedTick);
    [[nodiscard]] bool setUnmanned(
        ObjectId object, uint64_t confirmedTick);
    [[nodiscard]] bool setRepulsor(
        ObjectId object, bool repulsor, uint64_t confirmedTick);
    [[nodiscard]] bool setStealthEnabled(
        ObjectId object, bool enabled, uint64_t confirmedTick);
    [[nodiscard]] bool setPanelFlag(
        ObjectId object, ObjectPanelFlag flag, bool enabled,
        uint64_t confirmedTick);
    [[nodiscard]] bool setCaveIndex(ObjectId object, int32_t caveIndex);
    [[nodiscard]] bool setRailroadHeld(ObjectId object, bool held);
    [[nodiscard]] bool setStoppingDistance(
        ObjectId object, math::q32_32 distance);
    [[nodiscard]] bool setSupplyTruckIdleSuppressed(
        ObjectId object, bool suppressed, uint64_t confirmedTick);
    [[nodiscard]] bool assignSupplyTruckPreferredDock(
        ObjectId object, ObjectId center, uint64_t confirmedTick);
    [[nodiscard]] bool setWarehouseCashValue(
        ObjectId object, int32_t cashValue);
    [[nodiscard]] bool mutateSpecialPowerCountdown(
        ObjectId object, const SpecialPowerDefinition& definition,
        script::ScriptSpecialPowerCountdownOperation operation,
        int32_t seconds, bool paused, int32_t logicFramesPerSecond,
        uint64_t confirmedTick);
    [[nodiscard]] bool setAIAttitude(
        ObjectId object, ObjectAIAttitude attitude);
    [[nodiscard]] bool setAttackPrioritySetId(
        ObjectId object, uint32_t setId);
    [[nodiscard]] bool setScriptToppleDirection(
        ObjectId object, LogicFixedVec3 direction);
    [[nodiscard]] bool setDrawableCaption(
        ObjectId object, container::StringView text,
        uint64_t confirmedTick);
    [[nodiscard]] bool markSingleUseCommandUsed(
        ObjectId object, uint64_t confirmedTick);

private:
    [[nodiscard]] ecs::entity liveEntity(ObjectId object) const noexcept;

    ecs::registry& m_registry;
    ObjectLifecycle& m_objects;
};

} // namespace engine
