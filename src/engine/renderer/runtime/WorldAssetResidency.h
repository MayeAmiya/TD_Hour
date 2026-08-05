#pragma once

#include "core/container/hash_containers.h"
#include "engine/renderer/world/model/D3D12W3dModel.h"
#include "engine/renderer/world/model/W3dAnimationCache.h"
#include "engine/renderer/world/model/W3dAssetCache.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "engine/renderer/world/resource/WorldTextureOverrideCaches.h"
#include "engine/renderer/world/terrain/TerrainTextureResolver.h"

namespace engine::d3d12 { class D3D12Device; }

namespace engine::render {

// Long-lived renderer asset state. It survives individual frame preparation
// and view changes, but is reset/retired at explicit content or world-policy
// boundaries.
struct WorldAssetResidency final {
    explicit WorldAssetResidency(d3d12::D3D12Device& device);

    container::SharedPtr<WorldTextureCache> textures;
    SkyboxTextureOverrideCache skyboxTextureOverrides;
    TreeTextureOverrideCache treeTextureOverrides;
    W3dAssetCache assets;
    W3dAnimationCache animations;
    W3dRestPaletteFrameCache restPalettes;
    container::SharedPtr<const TerrainTextureResolver> terrainTextureResolver;
    container::HashSet<container::String> reportedAssetFailures;
    container::HashSet<container::String> reportedAnimationFailures;
    container::HashMap<container::String, W3dAssetVersion>
        registeredModelRevisions;
};

} // namespace engine::render
