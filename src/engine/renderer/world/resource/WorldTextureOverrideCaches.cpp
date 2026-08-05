#include "engine/renderer/world/resource/WorldTextureOverrideCaches.h"

#include "engine/renderer/world/resource/WorldTextureCache.h"

#include <string>

namespace engine::render {

bool SkyboxTextureOverrideCache::update(
    WorldTextureCache& cache,
    const container::Array<container::String,
                           kSkyboxMaterialFaceCount>& requested,
    container::String* error) {
    if (error) error->clear();
    if (initialized && textureNames == requested) return true;

    container::Array<uint32_t, kSkyboxMaterialFaceCount> acquiredSrvs{};
    size_t acquiredCount = 0;
    for (size_t index = 0; index < requested.size(); ++index) {
        const std::optional<uint32_t> srv = cache.acquire(requested[index]);
        if (!srv) {
            for (size_t releaseIndex = 0; releaseIndex < acquiredCount;
                 ++releaseIndex) {
                cache.release(requested[releaseIndex]);
            }
            if (error) {
                *error =
                    "GPU upload failed for WaterTransparency skybox face " +
                    std::to_string(index);
            }
            return false;
        }
        acquiredSrvs[index] = *srv;
        ++acquiredCount;
    }

    reset(cache);
    textureNames = requested;
    textureSrvs = acquiredSrvs;
    initialized = true;
    return true;
}

void SkyboxTextureOverrideCache::reset(WorldTextureCache& cache) {
    if (!initialized) return;
    for (const container::String& textureName : textureNames) {
        cache.release(textureName);
    }
    textureNames = {};
    textureSrvs = {};
    initialized = false;
}

void TreeTextureOverrideCache::beginEpoch(
    WorldTextureCache& cache, uint64_t presentationEpoch) {
    if (m_initialized && m_epoch == presentationEpoch) return;
    reset(cache);
    m_epoch = presentationEpoch;
    m_initialized = true;
}

std::optional<uint32_t> TreeTextureOverrideCache::acquire(
    WorldTextureCache& cache, container::StringView textureName,
    uint64_t presentationEpoch) {
    if (textureName.empty()) return std::nullopt;
    beginEpoch(cache, presentationEpoch);
    const container::String key(textureName);
    if (const auto found = m_textureSrvs.find(key);
        found != m_textureSrvs.end()) {
        return found->second;
    }
    const std::optional<uint32_t> srv = cache.acquire(textureName);
    if (!srv) return std::nullopt;
    m_textureSrvs.emplace(key, *srv);
    return srv;
}

void TreeTextureOverrideCache::reset(WorldTextureCache& cache) {
    for (const auto& [textureName, srv] : m_textureSrvs) {
        static_cast<void>(srv);
        cache.release(textureName);
    }
    m_textureSrvs.clear();
    m_epoch = 0;
    m_initialized = false;
}

} // namespace engine::render
