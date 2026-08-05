#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

struct ObjectRebuildHoleBehaviorRule final {
    uint32_t authoredOrder = 0;
    container::String workerTemplate;
    uint32_t workerRespawnDelayMilliseconds = 0;
    math::q32_32 healthRegenRatioPerSecond{0.1};
};

struct ObjectRebuildHoleExposeRule final {
    uint32_t authoredOrder = 0;
    container::String holeTemplate;
    math::q32_32 holeMaximumHealth{};
    bool transferAttackers = true;
};

struct ObjectRebuildHolePlan final {
    container::Vector<ObjectRebuildHoleBehaviorRule> behaviors;
    container::Vector<ObjectRebuildHoleExposeRule> exposes;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectRebuildHolePlan>
compileObjectRebuildHolePlan(const ThingTemplate& templateData);

} // namespace game
