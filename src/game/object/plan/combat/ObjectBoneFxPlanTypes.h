#pragma once

#include "core/container/container_types.h"

#include "game/base/DamageTypes.h"
#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace game {

struct ThingTemplate;

inline constexpr size_t kBoneFxStateCount = 4;
inline constexpr size_t kBoneFxMaximumSlots = 8;

enum class ObjectBoneFxPayloadKind : uint8_t {
    FxList,
    ObjectCreationList,
    ParticleSystem,
};

struct ObjectBoneFxEntry final {
    ObjectBoneFxPayloadKind kind = ObjectBoneFxPayloadKind::FxList;
    container::String boneName;
    container::String resource;
    math::q32_32 minimumDelayMilliseconds{};
    math::q32_32 maximumDelayMilliseconds{};
    uint8_t slot = 0;
    bool onlyOnce = true;
};

struct ObjectBoneFxRule final {
    container::Array<container::Vector<ObjectBoneFxEntry>, kBoneFxStateCount>
        entries;
    uint64_t fxDamageTypes = 0;
    uint64_t oclDamageTypes = 0;
    uint64_t particleDamageTypes = 0;
    uint32_t authoredOrder = 0;
};

struct ObjectBoneFxPlan final {
    container::Vector<ObjectBoneFxRule> rules;
    container::Vector<container::String> diagnostics;
    uint32_t damageModuleAuthoredOrder = 0;
    bool hasDamageModule = false;
};

[[nodiscard]] container::SharedPtr<const ObjectBoneFxPlan>
compileObjectBoneFxPlan(const ThingTemplate& templateData);

} // namespace game
