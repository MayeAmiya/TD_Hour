#pragma once

#include "ScriptCinematicPresentationControls.h"

#include <optional>

namespace engine::script {

// SET_INFANTRY_LIGHTING_OVERRIDE / RESET_INFANTRY_LIGHTING_OVERRIDE own one
// durable, session-scoped presentation setting.  `overrideScale == nullopt`
// is the original reset state: use the ordinary map/time-of-day infantry
// lighting instead of a script-provided multiplier.  The renderer receives a
// copied value through its sealed world snapshot and never reads a mutable
// GlobalData field while recording commands.
struct ScriptInfantryLightingPresentationState final {
    std::optional<float> overrideScale;
    ScriptPresentationControlStamp stamp{};
};

} // namespace engine::script
