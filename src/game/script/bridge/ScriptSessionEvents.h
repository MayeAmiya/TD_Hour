#pragma once

#include "core/container/container_types.h"

#include <cstdint>
namespace engine::script {

// Presentation-facing output of a confirmed script effect.  This is a
// session value queue, not a direct WND/renderer call; UI can consume it on
// the following presentation frame just as audio consumes GameAudioEvent.
enum class ScriptSessionEventKind : uint8_t {
    Text,
    CinematicText,
    Subtitle,
    MilitaryCaption,
    Diagnostic,
};

struct ScriptSessionEvent final {
    ScriptSessionEventKind kind = ScriptSessionEventKind::Text;
    uint64_t confirmedTick = 0;
    uint32_t sourceScriptId = 0;
    uint32_t ordinal = 0;
    container::String text;
    bool localized = true;
    // A raw authored font descriptor is meaningful only for CinematicText.
    // The presentation/UI layer parses it against its current font catalog;
    // no renderer or font handle crosses the confirmed script boundary.
    container::String fontDescriptor;
    // Zero means ordinary Text has no script-authored lifetime. Cinematic
    // text and Subtitle durations are confirmed logic ticks, never wall time.
    uint32_t durationTicks = 0;
    // SHOW_MILITARY_CAPTION is authored in milliseconds and its legacy UI
    // explicitly advances while script time is frozen. Keep that unit intact
    // for the client-local typewriter consumer instead of smuggling it into a
    // deterministic simulation timer.
    uint32_t durationMilliseconds = 0;
};

} // namespace engine::script
