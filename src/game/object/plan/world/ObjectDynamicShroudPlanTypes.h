#pragma once

#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <array>
#include <cstdint>

namespace game {

struct ThingTemplate;

// Pointer-free projection of RadiusDecalTemplate. The renderer does not yet
// own a persistent RadiusDecal system, but preserving this recipe now keeps
// gameplay migration from discarding stock presentation authoring.
struct ObjectDynamicShroudDecalRecipe final {
    container::String texture;
    uint32_t shadowTypeMask = 0x20u; // SHADOW_ALPHA_DECAL
    math::q32_32 minimumOpacity{int32_t{1}};
    math::q32_32 maximumOpacity{int32_t{1}};
    uint32_t opacityThrobMilliseconds = 1000;
    std::array<uint8_t, 4> color{0, 0, 0, 0};
    bool usesPlayerColor = true;
    bool onlyVisibleToOwningPlayer = true;
};

// One rule is retained for every final-recipe occurrence. Durations remain
// authored milliseconds here because the immutable archetype is compiled
// before a match selects its confirmed-frame rate; spawn converts them once.
struct ObjectDynamicShroudRule final {
    uint32_t authoredOrder = 0;
    uint32_t changeIntervalMilliseconds = 0;
    uint32_t growIntervalMilliseconds = 0;
    uint32_t shrinkDelayMilliseconds = 0;
    uint32_t shrinkTimeMilliseconds = 0;
    uint32_t growDelayMilliseconds = 0;
    uint32_t growTimeMilliseconds = 0;
    math::q32_32 finalVision{};
    ObjectDynamicShroudDecalRecipe gridDecal;
};

struct ObjectDynamicShroudPlan final {
    container::Vector<ObjectDynamicShroudRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectDynamicShroudPlan>
compileObjectDynamicShroudPlan(const ThingTemplate& templateData);

} // namespace game
