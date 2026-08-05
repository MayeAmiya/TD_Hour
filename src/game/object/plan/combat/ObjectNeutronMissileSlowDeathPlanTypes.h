#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>

namespace game {
struct ThingTemplate;

struct ObjectNeutronMissileBlastRule final {
    // RefCode uses parseDurationReal, so preserve fractional milliseconds
    // until the fixed logic-frame conversion instead of rounding at load.
    math::q32_32 delayMilliseconds{};
    math::q32_32 scorchDelayMilliseconds{};
    math::q32_32 innerRadius{};
    math::q32_32 outerRadius{};
    math::q32_32 maximumDamage{};
    math::q32_32 minimumDamage{};
    math::q32_32 toppleSpeed{};
    math::q32_32 pushForce{};
    bool enabled = false;
};

// RefCode's NeutronMissileSlowDeathBehavior authors Blast1..Blast9, so the
// compiled rule keeps a fixed extent.  The simulation tracks per-blast
// completion in a bitmask, so raising this count requires widening
// engine::ObjectNeutronMissileBlastMask as well; a static_assert beside that
// type enforces the coupling.
inline constexpr size_t kObjectNeutronMissileBlastCount = 9;

struct ObjectNeutronMissileSlowDeathRule final {
    uint32_t authoredOrder = 0;
    math::q32_32 scorchMarkSize{};
    container::String fxList;
    container::Array<ObjectNeutronMissileBlastRule,
                     kObjectNeutronMissileBlastCount> blasts;
};

struct ObjectNeutronMissileSlowDeathPlan final {
    container::Vector<ObjectNeutronMissileSlowDeathRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectNeutronMissileSlowDeathPlan>
compileObjectNeutronMissileSlowDeathPlan(const ThingTemplate& templateData);
}

