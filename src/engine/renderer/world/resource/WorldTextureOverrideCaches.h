#pragma once

#include "core/container/hash_containers.h"
#include "engine/renderer/world/effects/SkyboxMaterialOverrides.h"

#include <cstdint>
#include <optional>

namespace engine::render {

class WorldTextureCache;

class SkyboxTextureOverrideCache final {
public:
    [[nodiscard]] bool update(
        WorldTextureCache& cache,
        const container::Array<container::String,
                               kSkyboxMaterialFaceCount>& requested,
        container::String* error = nullptr);
    void reset(WorldTextureCache& cache);

    container::Array<container::String, kSkyboxMaterialFaceCount> textureNames{};
    container::Array<uint32_t, kSkyboxMaterialFaceCount> textureSrvs{};
    bool initialized = false;
};

class TreeTextureOverrideCache final {
public:
    void beginEpoch(WorldTextureCache& cache, uint64_t presentationEpoch);
    [[nodiscard]] std::optional<uint32_t> acquire(
        WorldTextureCache& cache, container::StringView textureName,
        uint64_t presentationEpoch);
    void reset(WorldTextureCache& cache);

private:
    container::HashMap<container::String, uint32_t> m_textureSrvs;
    uint64_t m_epoch = 0;
    bool m_initialized = false;
};

} // namespace engine::render
