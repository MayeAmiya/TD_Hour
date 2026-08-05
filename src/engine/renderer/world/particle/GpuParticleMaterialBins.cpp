#include "engine/renderer/world/particle/GpuParticleMaterialBins.h"

#include "core/container/hash_containers.h"

#include <algorithm>
#include <string>
#include <utility>

namespace engine::render {

namespace {

[[nodiscard]] char asciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

} // namespace

container::String gpuParticleMaterialBinKey(
    fx::ParticleShader shader, container::StringView textureName) {
    container::String key = std::to_string(static_cast<uint32_t>(shader));
    key.push_back('|');
    key.append(textureName);
    for (char& value : key) {
        value = asciiLower(value);
    }
    return key;
}

bool gpuParticleMaterialBinMatches(
    const GpuParticleMaterialBin& bin, fx::ParticleShader shader,
    container::StringView textureName) noexcept {
    if (bin.shader != shader || bin.textureName.size() != textureName.size()) {
        return false;
    }
    for (size_t index = 0; index < textureName.size(); ++index) {
        if (asciiLower(bin.textureName[index]) !=
            asciiLower(textureName[index])) {
            return false;
        }
    }
    return true;
}

GpuParticleMaterialBinCompilation compileGpuParticleMaterialBins(
    const fx::ParticleSystemCatalog& catalog, size_t maximumBins) {
    GpuParticleMaterialBinCompilation result;
    const auto& templates = catalog.templates();
    result.templateToBin.assign(templates.size() + 1u, UINT32_MAX);

    container::HashMap<container::String, uint32_t> binByMaterial;
    binByMaterial.reserve(std::min(maximumBins, templates.size()));
    for (size_t index = 0; index < templates.size(); ++index) {
        const fx::ParticleSystemTemplate& definition = templates[index];
        if (!definition.gpuCompatible()) continue;

        container::String materialKey = gpuParticleMaterialBinKey(
            definition.shader, definition.particleName);

        uint32_t binIndex = UINT32_MAX;
        if (const auto found = binByMaterial.find(materialKey);
            found != binByMaterial.end()) {
            binIndex = found->second;
        } else if (result.bins.size() < maximumBins) {
            binIndex = static_cast<uint32_t>(result.bins.size());
            result.bins.push_back({
                .shader = definition.shader,
                .textureName = definition.particleName,
                .firstTemplateId = static_cast<uint32_t>(index + 1u),
            });
            binByMaterial.emplace(std::move(materialKey), binIndex);
        } else {
            ++result.rejectedCompatibleTemplates;
            continue;
        }
        result.templateToBin[index + 1u] = binIndex;
        ++result.mappedTemplates;
    }
    return result;
}

} // namespace engine::render
