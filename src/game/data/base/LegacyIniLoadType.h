#pragma once

#include <cstdint>

namespace game::ini {

// Mirrors the load policy observed by Zero Hour definition parsers.  VFS
// selection is deliberately outside this enum: one logical path has already
// been reduced to its winner before a catalog sees any blocks.
enum class LegacyIniLoadType : uint8_t {
    Overwrite,
    CreateOverrides,
    Multifile,
};

[[nodiscard]] constexpr bool createsOverrides(
    LegacyIniLoadType type) noexcept {
    return type == LegacyIniLoadType::CreateOverrides;
}

} // namespace game::ini
