#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace game {

struct ThingTemplate;
namespace terrain { struct MapVisibilitySnapshot; }

enum class ObjectBuilderKind : uint8_t {
    Dozer,
    Worker,
};

struct ObjectBuilderRule final {
    uint32_t authoredOrder = 0;
    ObjectBuilderKind kind = ObjectBuilderKind::Dozer;
    math::q32_32 repairHealthRatioPerSecond{};
    uint32_t boredTimeMilliseconds = 0;
    math::q32_32 boredRange{};
    uint32_t maxBoxes = 0;
    uint32_t supplyCenterActionDelayMilliseconds = 0;
    uint32_t supplyWarehouseActionDelayMilliseconds = 0;
    math::q32_32 supplyWarehouseScanDistance{100};
    container::String suppliesDepletedVoice;
    uint32_t upgradedSupplyBoost = 0;
};

struct ObjectBuilderPlan final {
    container::Vector<ObjectBuilderRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectBuilderPlan>
compileObjectBuilderPlan(const ThingTemplate& templateData);

} // namespace game
