#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"

#include <array>
#include <cstdint>

namespace game {

struct ThingTemplate;

enum class ObjectDynamicGeometryKind : uint8_t {
    Basic,
    Firestorm,
};

// FirestormDynamicGeometryInfoUpdate is the only stock subclass of the
// generic geometry writer.  Preserve all sixteen authored particle slots;
// their stable indices are part of the original module's presentation
// identity even when most stock recipes populate only the first two.
struct ObjectFirestormGeometryRecipe final {
    std::array<container::String, 16> particleSystems;
    container::String fxList;
    math::q32_32 particleOffsetZ{};
    math::q32_32 scorchSize{};
    math::q32_32 damageIntervalMilliseconds{};
    math::q32_32 damageAmount{};
    math::q32_32 maximumHeightForDamage{int32_t{20}};
};

// Immutable final-recipe projection. Durations remain authored milliseconds
// because ThingFactory compiles before a session chooses its logic rate.
// An absent TransitionTime deliberately means one confirmed frame, matching
// DynamicGeometryInfoUpdateModuleData's legacy constructor default.
struct ObjectDynamicGeometryRule final {
    uint32_t authoredOrder = 0;
    ObjectDynamicGeometryKind kind = ObjectDynamicGeometryKind::Basic;
    uint32_t initialDelayMilliseconds = 0;
    math::q32_32 initialHeight{};
    math::q32_32 initialMajorRadius{};
    math::q32_32 initialMinorRadius{};
    math::q32_32 finalHeight{};
    math::q32_32 finalMajorRadius{};
    math::q32_32 finalMinorRadius{};
    uint32_t transitionMilliseconds = 0;
    bool hasAuthoredTransitionTime = false;
    bool reverseAtTransitionTime = false;
    ObjectFirestormGeometryRecipe firestorm;
};

struct ObjectDynamicGeometryPlan final {
    container::Vector<ObjectDynamicGeometryRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectDynamicGeometryPlan>
compileObjectDynamicGeometryPlan(const ThingTemplate& templateData);

} // namespace game

namespace game::terrain {
class TerrainLogic;
}
