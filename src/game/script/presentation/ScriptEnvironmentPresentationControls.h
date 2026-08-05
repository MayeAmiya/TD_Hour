#pragma once

#include "core/container/container_types.h"

#include "game/data/presentation/ScriptWeatherPresentationSettings.h"

#include "ScriptCinematicPresentationControls.h"

#include <cstdint>
namespace engine::script {

// SET_TREE_SWAY owns the current map-wide breeze parameters, while each
// SwayClientUpdate object owns its own visual phase.  Keep this value at the
// confirmed presentation boundary: it is deliberately not SimulationRandom
// state and it never carries a renderer object or an ECS entity.
//
// The defaults are ScriptEngine::reset's stock Generals values.  `enabled`
// is the captured client tree-sway preference; objects without an explicit
// SwayClientUpdate opt-in remain unaffected regardless of this flag.
struct ScriptTreeSwayPresentationState final {
    bool enabled = true;
    float directionRadians = math::PI / 3.0f;
    float intensityRadians = 0.07f * math::PI / 4.0f;
    float leanRadians = 0.07f * math::PI / 4.0f;
    uint32_t periodFrames = 150;
    float randomness = 0.2f;
    ScriptPresentationControlStamp stamp{};
};

// SHOW_WEATHER owns only `visible`.  `snow.enabled` remains the Weather.ini
// capability flag, so SHOW_WEATHER(true) correctly remains a no-op on maps
// with no configured weather effect.
struct ScriptWeatherPresentationState final {
    bool visible = true;
    ScriptWeatherSnowSettings snow{};
    ScriptPresentationControlStamp stamp{};
};

} // namespace engine::script
