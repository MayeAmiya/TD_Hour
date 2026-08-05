#include "core/container/container_types.h"
#include "engine/renderer/world/effects/SkyboxMaterialOverrides.h"

#include <algorithm>
#include <cctype>
namespace engine::render {
namespace {

[[nodiscard]] container::String canonicalToken(container::StringView value) {
    const size_t slash = value.find_last_of("/\\");
    if (slash != container::StringView::npos) value.remove_prefix(slash + 1);
    const size_t extension = value.find_last_of('.');
    if (extension != container::StringView::npos) value = value.substr(0, extension);

    container::String result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character)) {
            result.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return result;
}

[[nodiscard]] int32_t faceFromLegacyTexture(container::StringView textureName) {
    static constexpr container::Array<container::StringView, kSkyboxMaterialFaceCount> faces = {
        "tsmorningn", "tsmorninge", "tsmornings", "tsmorningw", "tsmorningt"};
    const container::String token = canonicalToken(textureName);
    for (size_t index = 0; index < faces.size(); ++index) {
        if (token == faces[index]) return static_cast<int32_t>(index);
    }
    return -1;
}

} // namespace

SkyboxMaterialFaceMap resolveSkyboxMaterialFaces(
    container::Span<const W3dMaterialTextureBinding> materials) {
    SkyboxMaterialFaceMap result{};
    result.fill(-1);
    container::Vector<uint32_t> usedMaterials;
    usedMaterials.reserve(materials.size());

    const auto bind = [&result, &usedMaterials](int32_t face,
                                                uint32_t materialIndex) {
        if (face < 0 || static_cast<size_t>(face) >= result.size() ||
            result[static_cast<size_t>(face)] >= 0 ||
            std::find(usedMaterials.begin(), usedMaterials.end(), materialIndex) !=
                usedMaterials.end()) {
            return;
        }
        result[static_cast<size_t>(face)] = static_cast<int32_t>(materialIndex);
        usedMaterials.push_back(materialIndex);
    };

    // This is the exact RefCode path: WaterTransparency begins with
    // TSMorningN/E/S/W/T, and INIWater later replaces those prototype texture
    // names on its one new_skybox instance. Do not infer a face for other
    // source names: RefCode never uses material labels or slot order here.
    for (const W3dMaterialTextureBinding& material : materials) {
        bind(faceFromLegacyTexture(material.textureName), material.materialIndex);
    }
    return result;
}

container::Vector<W3dMaterialTextureOverride> makeSkyboxMaterialTextureOverrides(
    container::Span<const W3dMaterialTextureBinding> materials,
    container::Span<const uint32_t, kSkyboxMaterialFaceCount> textureSrvs) {
    constexpr uint8_t kClampUvSamplerMode = 3;
    const SkyboxMaterialFaceMap materialFaces = resolveSkyboxMaterialFaces(materials);

    container::Vector<W3dMaterialTextureOverride> overrides;
    overrides.reserve(materialFaces.size());
    for (size_t face = 0; face < materialFaces.size(); ++face) {
        if (materialFaces[face] < 0) continue;
        overrides.push_back({
            .materialIndex = static_cast<uint32_t>(materialFaces[face]),
            .textureSrvIndex = textureSrvs[face],
            .samplerMode = kClampUvSamplerMode,
        });
    }
    return overrides;
}

} // namespace engine::render
