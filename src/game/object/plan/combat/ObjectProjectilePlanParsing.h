#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {
struct ModuleData;
struct ThingTemplate;
}

namespace game::object_projectile_plan_detail {

using Fixed = math::q32_32;

constexpr uint32_t kDefaultDumbProjectileLifespanMilliseconds = 10'000;
inline const Fixed kFixedZero{int32_t{0}};

[[nodiscard]] const ModuleData* findModule(
    const ThingTemplate& templateData,
    container::StringView moduleClass) noexcept;
[[nodiscard]] bool hasModule(const ThingTemplate& templateData,
                             container::StringView moduleClass) noexcept;
[[nodiscard]] const container::String* moduleValue(
    const ModuleData& module, container::StringView key) noexcept;
[[nodiscard]] const ModuleData* moduleChild(
    const ModuleData& module,
    container::StringView childClass) noexcept;
[[nodiscard]] uint32_t parseDecalStyle(
    container::StringView text, uint32_t fallback = 0x20u) noexcept;
[[nodiscard]] container::Array<uint8_t, 4> parseDecalColor(
    container::StringView text,
    container::Array<uint8_t, 4> fallback = {0, 0, 0, 0}) noexcept;
[[nodiscard]] float parseFiniteFloat(
    container::StringView text, float fallback) noexcept;
[[nodiscard]] float parsePercentToUnit(
    container::StringView text, float fallback) noexcept;
[[nodiscard]] uint32_t parseMilliseconds(
    container::StringView text, uint32_t fallback) noexcept;
[[nodiscard]] uint32_t parseUnsigned(
    container::StringView text, uint32_t fallback) noexcept;
[[nodiscard]] bool parseBool(
    container::StringView text, bool fallback) noexcept;
[[nodiscard]] Fixed fixedFinite(
    float value, float fallback = 0.0f) noexcept;

} // namespace game::object_projectile_plan_detail
