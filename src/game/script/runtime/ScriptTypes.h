#pragma once

#include "core/container/hash_containers.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include "game/script/runtime/ScriptCommandBarOverrideTypes.h"
#include "game/script/runtime/ScriptExecutionContext.h"
#include "game/script/runtime/ScriptGameplayEventLedger.h"
#include "math/fixed/q32_32.h"
#include "core/math/wwmath/base/wwmath.h"

namespace engine::script
{

// Authoritative script coordinates are quantized while the immutable
// program is compiled. Presentation-only script payloads continue to use
// math::vec3 at their client boundary.
struct ScriptFixedVec3 final
{
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 z{};
};

// The legacy ScriptEngine addressed scripts by mutable pointers and names.
// Runtime programs instead use stable, serializable IDs.  Zero is never a
// valid authored reference, which makes absent references explicit at every
// value boundary.
struct ScriptId final
{
    uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }
    [[nodiscard]] constexpr auto operator<=>(const ScriptId&) const noexcept = default;
};

struct ScriptGroupId final
{
    uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }
    [[nodiscard]] constexpr auto operator<=>(const ScriptGroupId&) const noexcept = default;
};

inline constexpr ScriptId INVALID_SCRIPT_ID{};
inline constexpr ScriptGroupId INVALID_SCRIPT_GROUP_ID{};

// Program-local, immutable key for the small pieces of mutable ScriptRuntime
// state (flags, counters and timers).  The compiler assigns IDs from a
// lexically canonical table; runtime code consequently never needs to sort
// or compare a string while evaluating a compiled condition/action.
//
// Counter/timer and flag tables are intentionally separate namespaces.  The
// original engine permits the same spelling to be used once as a counter and
// once as a flag, and those values must remain independent.
struct ScriptRuntimeSymbolId final
{
    uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value != 0;
    }
    [[nodiscard]] constexpr auto operator<=>(const ScriptRuntimeSymbolId&) const noexcept = default;
};

inline constexpr ScriptRuntimeSymbolId INVALID_SCRIPT_RUNTIME_SYMBOL_ID{};
// A ScriptList loaded outside a SidesList (for example a standalone .scb
// helper file) has no map-side context.  It must not accidentally bind to
// source Side zero when a session later installs scenario player bindings.
inline constexpr uint32_t INVALID_LEGACY_SIDE_ORDINAL = 0xffffffffu;
inline constexpr uint32_t INVALID_LEGACY_BUILD_LIST_ORDINAL = 0xffffffffu;

enum class ScriptDifficulty : uint8_t
{
    Easy,
    Normal,
    Hard,
};

struct ScriptDifficultyMask final
{
    bool easy = true;
    bool normal = true;
    bool hard = true;

    [[nodiscard]] constexpr bool includes(ScriptDifficulty difficulty) const noexcept
    {
        switch (difficulty)
        {
        case ScriptDifficulty::Easy:
            return easy;
        case ScriptDifficulty::Normal:
            return normal;
        case ScriptDifficulty::Hard:
            return hard;
        }
        return false;
    }
};

enum class ScriptComparison : uint8_t
{
    Less,
    LessEqual,
    Equal,
    GreaterEqual,
    Greater,
    NotEqual,
};

} // namespace engine::script
