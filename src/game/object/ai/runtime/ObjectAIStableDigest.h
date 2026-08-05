#pragma once

#include <cstdint>

namespace engine::ai
{

struct ObjectAIRuntimeSnapshot;

class ObjectAIStableDigest final
{
public:
    // 21: ObjectAIReadOnlyFact carries the JetAIUpdate identity used by the
    // original Guard/GuardRetaliate out-of-ammo gate.
    // 18: ObjectAIReadOnlyFact separates current-locomotor availability from
    // ground movement. Airborne movers use the former but must not enter the
    // ground-path branch.
    // 17: ObjectAIReadOnlyFact carries the repulsor reactor inputs -- the
    // KINDOF_CAN_BE_REPULSED gate, the nearest repulsor inside vision range,
    // and the authored locomotor wander radius/width factor.
    // 16: ObjectAIReadOnlyFact carries the recent hostile damage source that
    // the Guard family retaliates against.
    static constexpr uint32_t EncodingVersion = 21;

    [[nodiscard]] static uint64_t compute(const ObjectAIRuntimeSnapshot& snapshot) noexcept;
};

[[nodiscard]] uint64_t stableDigest(const ObjectAIRuntimeSnapshot& snapshot) noexcept;

} // namespace engine::ai
