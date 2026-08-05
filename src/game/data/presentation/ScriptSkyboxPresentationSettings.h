#pragma once

#include "core/container/container_types.h"
#include <cstddef>
namespace engine::script {

// RefCode 的 WaterTransparencySetting 固定拥有这五个具名天空盒面。
// faces. Keep the original N/E/S/W/T order stable across data loading,
// session freezing and renderer extraction; it is not a cube-map coordinate
// convention that a backend may silently reorder.
inline constexpr size_t kScriptSkyboxTextureFaceCount = 5;

struct ScriptSkyboxTextureSet final {
    // Water.h's WaterTransparencySetting defaults. A shipped Water.ini may
    // omit the entire block (and the current extracted Zero Hour file does),
    // so these must be useful content defaults rather than empty sentinels.
    container::Array<container::String, kScriptSkyboxTextureFaceCount> textureNames{
        "TSMorningN.tga",
        "TSMorningE.tga",
        "TSMorningS.tga",
        "TSMorningW.tga",
        "TSMorningT.tga",
    };
};

} // namespace engine::script
