#pragma once

#include "engine/renderer/world/resource/WorldTextureDecodeService.h"

namespace engine::render::detail {

enum class WorldTextureVariantKind : uint8_t {
    Ordinary,
    TerrainColor,
    TerrainAlphaEdge,
};

struct WorldTextureDecodeRequest final {
    container::String logicalName;
    container::String sourceKey;
    container::String variantKey;
    WorldTextureVariantKind kind = WorldTextureVariantKind::Ordinary;
    WorldTextureVariant variant = WorldTextureVariant::ColorLegacyGamma;
    uint32_t gridWidth = 0;
    uint32_t reduction = 0;
    uint64_t policyGeneration = 0;
    uint64_t enqueueSequence = 0;
    uint64_t estimatedBytes = 256u * 1024u;
    uint32_t deferredPasses = 0;
    RenderAssetPriority priority = RenderAssetPriority::Normal;
};

struct WorldTextureDecodeJob final {
    enum class Kind : uint8_t { DecodeSource, BuildVariant };

    Kind kind = Kind::DecodeSource;
    WorldTextureDecodeRequest request;
    container::SharedPtr<const RawTexture> source;
    container::SharedPtr<TextureManager> decoder;
    uint64_t generation = 0;
};

struct WorldTextureDecodeCompletion final {
    WorldTextureDecodeJob::Kind kind =
        WorldTextureDecodeJob::Kind::DecodeSource;
    WorldTextureDecodeRequest request;
    container::SharedPtr<const RawTexture> source;
    container::SharedPtr<const WorldTextureDecodeService::Lookup::Payload>
        payload;
    container::String canonicalSourceKey;
    container::String diagnostic;
    TextureManagerStats stats;
    container::SharedPtr<TextureManager> decoder;
    uint64_t generation = 0;
    uint64_t schedulerSequence = 0;
    uint64_t workerNanoseconds = 0;
};

// Executes one immutable decode/build job on a resource worker. It does not
// publish into cache maps or mutate scheduler admission state.
class WorldTextureDecodeWorker final {
public:
    [[nodiscard]] static WorldTextureDecodeCompletion run(
        WorldTextureDecodeJob job) noexcept;
};

} // namespace engine::render::detail
