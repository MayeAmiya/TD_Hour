#pragma once

#include "core/container/container_types.h"
#include <cstddef>
namespace engine::script {

inline constexpr size_t kScriptWaterTimeOfDayCount = 4;

struct ScriptWaterColor final {
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    float alpha = 1.0f;
};

struct ScriptWaterTimeOfDaySettings final {
    container::String skyTexture;
    container::String waterTexture = "TSWater.tga";
    container::Array<ScriptWaterColor, 4> vertexColors{};
    ScriptWaterColor diffuseColor;
    ScriptWaterColor transparentDiffuseColor;
    float uScrollPerMillisecond = 0.0f;
    float vScrollPerMillisecond = 0.0f;
    float skyTexelsPerUnit = 0.0f;
    int waterRepeatCount = 32;
};

// Presentation-only WaterSet/WaterTransparency content. GameSession freezes
// this value at map start so rendering never reaches back into mutable VFS or
// process-global INI state.
struct ScriptWaterPresentationSettings final {
    container::Array<ScriptWaterTimeOfDaySettings, kScriptWaterTimeOfDayCount> timeOfDay;
    float transparentWaterDepth = 3.0f;
    float minimumWaterOpacity = 1.0f;
    ScriptWaterColor standingWaterColor;
    ScriptWaterColor radarWaterColor{0.55f, 0.55f, 1.0f, 1.0f};
    container::String standingWaterTexture = "TWWater01.tga";
    bool additiveBlending = false;

    ScriptWaterPresentationSettings();
};

// Applies one INI layer. Missing fields inherit the existing value, matching
// RefCode's base-to-override WaterTransparency chain and sparse map overrides.
[[nodiscard]] bool applyScriptWaterPresentationIni(
    container::StringView content,
    ScriptWaterPresentationSettings& settings,
    container::String* error = nullptr);

} // namespace engine::script
