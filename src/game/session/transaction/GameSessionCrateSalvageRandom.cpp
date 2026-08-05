#include "game/session/transaction/GameSessionCrateSalvageRandom.h"

#include "game/object/simulation/status/ObjectCrateCollide.h"

#include <limits>

namespace engine::crate_salvage {
namespace {

[[nodiscard]] uint64_t mix(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t key(
    uint64_t sessionSeed,
    const ObjectCratePickupCommand& command,
    uint64_t purpose) noexcept {
    uint64_t value = mix(sessionSeed);
    value ^= mix(static_cast<uint64_t>(command.crate.value));
    value ^= mix(static_cast<uint64_t>(command.picker.value) << 32u |
                 static_cast<uint64_t>(command.authoredOrder));
    value ^= mix(command.confirmedTick);
    return mix(value ^ purpose);
}

[[nodiscard]] math::q32_32 randomUnit(
    uint64_t sessionSeed,
    const ObjectCratePickupCommand& command,
    uint64_t purpose) noexcept {
    constexpr uint64_t UnitRaw = uint64_t{1} << 32u;
    constexpr uint64_t MaximumSample =
        std::numeric_limits<uint32_t>::max();
    const uint64_t sample = static_cast<uint32_t>(
        key(sessionSeed, command, purpose));
    return math::q32_32::from_raw(static_cast<int64_t>(
        (sample * UnitRaw) / MaximumSample));
}

} // namespace

bool chanceSucceeds(
    uint64_t sessionSeed,
    const ObjectCratePickupCommand& command,
    math::q32_32 chance,
    uint64_t purpose) noexcept {
    const math::q32_32 one{int32_t{1}};
    return chance == one || randomUnit(sessionSeed, command, purpose) < chance;
}

int32_t randomInteger(
    uint64_t sessionSeed,
    const ObjectCratePickupCommand& command,
    int32_t minimum,
    int32_t maximum,
    uint64_t purpose) noexcept {
    if (minimum == maximum) return minimum;
    if (minimum > maximum) return maximum;
    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(maximum) - static_cast<int64_t>(minimum)) + 1u;
    const uint64_t offset = key(sessionSeed, command, purpose) % width;
    return static_cast<int32_t>(
        static_cast<int64_t>(minimum) + static_cast<int64_t>(offset));
}

} // namespace engine::crate_salvage
