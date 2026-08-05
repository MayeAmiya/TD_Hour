#pragma once

#include "core/container/container_types.h"

#include "engine/renderer/world/model/D3D12W3dModel.h"
#include <cstddef>
#include <cstdint>
namespace engine::render {

// RefCode WaterTransparency has five ordered fields: N, E, S, W, T. The
// returned array maps each face index to one immutable new_skybox material
// index, or -1 when a malformed/custom W3D does not expose that face.
inline constexpr size_t kSkyboxMaterialFaceCount = 5;
using SkyboxMaterialFaceMap = container::Array<int32_t, kSkyboxMaterialFaceCount>;

// Resolves the W3D material slots which RefCode's WaterTransparency setter
// actually replaces: a material must still name one of the five default
// TSMorning face textures. Unrelated/custom material names are deliberately
// not guessed or reordered, so they retain the model's original SRV.
[[nodiscard]] SkyboxMaterialFaceMap resolveSkyboxMaterialFaces(
    container::Span<const W3dMaterialTextureBinding> materials);

// Builds transient packet overrides for the matched RefCode faces. Every
// replacement forces both U and V to clamp, matching W3DWater's explicit
// post-replacement sampler update. The shared W3D material table is not
// mutated.
[[nodiscard]] container::Vector<W3dMaterialTextureOverride>
makeSkyboxMaterialTextureOverrides(
    container::Span<const W3dMaterialTextureBinding> materials,
    container::Span<const uint32_t, kSkyboxMaterialFaceCount> textureSrvs);

} // namespace engine::render
