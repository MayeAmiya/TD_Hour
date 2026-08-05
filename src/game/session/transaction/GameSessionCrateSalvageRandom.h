#pragma once

#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {
struct ObjectCratePickupCommand;

namespace crate_salvage {

[[nodiscard]] bool chanceSucceeds(
    uint64_t sessionSeed,
    const ObjectCratePickupCommand& command,
    math::q32_32 chance,
    uint64_t purpose) noexcept;

[[nodiscard]] int32_t randomInteger(
    uint64_t sessionSeed,
    const ObjectCratePickupCommand& command,
    int32_t minimum,
    int32_t maximum,
    uint64_t purpose) noexcept;

} // namespace crate_salvage
} // namespace engine
