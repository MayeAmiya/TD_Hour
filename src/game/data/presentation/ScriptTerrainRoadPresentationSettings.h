#pragma once

#include "core/container/container_types.h"
#include "math/fixed/q32_32.h"
#include <cstdint>
namespace engine::script {

struct ScriptTerrainRoadStyle final {
    container::String name;
    container::String texture;
    // TerrainRoadType starts at zero, then new named roads copy the current
    // DefaultRoad values at creation time.  Keep a stable catalog identity so
    // renderer endpoint matching does not depend on spelling or later sparse
    // INI overlays.
    uint32_t identity = 0;
    float width = 0.0f;
    float widthInTexture = 0.0f;
};

struct ScriptTerrainBridgeStyle final {
    container::String name;
    float scale = 0.7f;
    math::q32_32 scaleFixed = math::q32_32::from_fraction(7, 10);
    container::Array<float, 3> radarColor{};
    container::Array<container::String, 4> modelNames;
    container::Array<container::String, 4> textureNames;
    container::Array<container::String, 4> towerObjectNames;
    container::String scaffoldObjectName;
    container::String scaffoldSupportObjectName;
    float transitionEffectsHeight = 0.0f;
    math::q32_32 transitionEffectsHeightFixed{};
    int32_t effectsPerType = 0;
};

struct ScriptTerrainRoadPresentationSettings final {
    container::Vector<ScriptTerrainRoadStyle> roads;
    container::Vector<ScriptTerrainBridgeStyle> bridges;
    uint32_t nextRoadIdentity = 1;

    [[nodiscard]] const ScriptTerrainRoadStyle* find(
        container::StringView name) const noexcept;
    [[nodiscard]] const ScriptTerrainBridgeStyle* findBridge(
        container::StringView name) const noexcept;
};

[[nodiscard]] bool applyScriptTerrainRoadPresentationIni(
    container::StringView content,
    ScriptTerrainRoadPresentationSettings& settings,
    container::String* error = nullptr);

} // namespace engine::script
