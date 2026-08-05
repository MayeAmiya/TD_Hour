#pragma once

#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <cstdint>
namespace game {

struct ThingTemplate;

// Immutable, final-recipe projection of HeightDieUpdate.  The legacy class
// has no mutable content fields: every occurrence shares this value recipe,
// while its delay/direction/death latch lives per ECS entity below.
struct ObjectHeightDieRule final {
    uint32_t authoredOrder = 0;
    math::q32_32 targetHeightAboveTerrain{};
    bool targetHeightIncludesStructures = false;
    bool onlyWhenMovingDown = false;
    // RefCode compares world Z against this value (despite its historical
    // comment saying "above terrain").  Keep it fixed-point and distinct
    // from TargetHeight so a future presentation particle owner can consume
    // the exact authored threshold.
    math::q32_32 destroyAttachedParticlesAtHeight{int32_t{-1}};
    bool snapToGroundOnDeath = false;
    // Authoring is milliseconds; the confirmed fixed-step system converts it
    // with the same round-up convention as parseDurationUnsignedInt.
    uint32_t initialDelayMilliseconds = 0;
};

struct ObjectHeightDiePlan final {
    container::Vector<ObjectHeightDieRule> rules;
    // Typed parsing reports invalid content to ThingFactory before it can
    // become a silent zero-height/zero-delay live behavior.
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectHeightDiePlan>
compileObjectHeightDiePlan(const ThingTemplate& templateData);

} // namespace game

namespace game::terrain {
class TerrainLogic;
}
