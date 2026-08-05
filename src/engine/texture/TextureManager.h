#pragma once

#include "core/container/hash_containers.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include "io/VFS.h"

namespace engine {

struct MappedImage;

enum class RawTextureGpuFormat : uint8_t {
    Rgba8,
    Bc1,
    Bc2,
    Bc3,
};

struct RawTextureMip final {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t byteOffset = 0;
    uint32_t rowPitch = 0;
    uint32_t slicePitch = 0;
};

// CPU callers keep a normalized top-level RGBA image. The renderer consumes
// gpuPixels/mips directly so authored BC DDS payloads and generated TGA mips
// do not have to be expanded into a one-level GPU texture.
struct RawTexture : public std::enable_shared_from_this<RawTexture> {
    container::Vector<uint8_t> pixels;
    container::Vector<uint8_t> gpuPixels;
    container::Vector<RawTextureMip> mips;
    container::String sourcePath;
    // Process-unique renderer identity assigned by TextureManager. Unlike the
    // object address, this value is never reused after destroyAll()/reload.
    uint64_t rendererIdentity = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    RawTextureGpuFormat gpuFormat = RawTextureGpuFormat::Rgba8;
    bool hasData() const { return !pixels.empty(); }
    bool hasGpuData() const { return !gpuPixels.empty() && !mips.empty(); }

    // UI draw lists cross the main/render thread boundary. A lease keeps this
    // immutable texture generation alive until the render thread has replayed
    // the frame; stack-owned diagnostic textures simply return an empty lease.
    [[nodiscard]] container::SharedPtr<const RawTexture> lease() const noexcept {
        return weak_from_this().lock();
    }
};

// Result of looking up a mapped image name
struct MappedImageResult {
    const RawTexture* texture = nullptr;
    int left = 0, top = 0, right = 0, bottom = 0;
    int texW = 0, texH = 0;
    bool found = false;
};

struct TextureManagerStats final {
    size_t decodedSources = 0;
    size_t proceduralSources = 0;
    size_t aliases = 0;
    size_t negativeLookups = 0;
    uint64_t cpuBytes = 0;
    uint64_t requests = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t decodeAttempts = 0;
    uint64_t decodeSucceeded = 0;
    uint64_t decodeFailed = 0;
    uint64_t missingSources = 0;
    uint64_t unsupportedSources = 0;
    uint64_t resets = 0;
    uint64_t generation = 1;
};

class TextureManager {
public:
    TextureManager();   // No SDL_Renderer needed
    ~TextureManager();

    // Load a texture by filename → returns RawTexture (or nullptr on failure)
    const RawTexture* loadTexture(const container::String& filename);
    [[nodiscard]] container::String resolveTextureIdentity(const container::String& filename);

    // Look up a mapped image name → MappedImageResult (caller resolves texture)
    MappedImageResult findMappedImage(const container::String& name);

    void destroyAll();

    // Placeholder texture for missing textures
    const RawTexture* getPlaceholder();

    // Create a cached solid-color 1x1 texture
    const RawTexture* getOrCreateSolid(const container::String& key, uint32_t argbColor);

    // Presentation-owned procedural RGBA texture. Every update publishes a
    // fresh immutable generation; an in-flight UI frame may retain the old
    // generation without racing the producer or extending a GPU identity.
    const RawTexture* updateRuntimeRgbaTexture(
        const container::String& key, uint32_t width, uint32_t height,
        container::Vector<uint8_t> rgbaPixels);

    // Direct access to raw texture cache
    const RawTexture* getCached(const container::String& name) const;
    [[nodiscard]] TextureManagerStats stats() const noexcept;

private:
    [[nodiscard]] static uint64_t allocateRendererIdentity() noexcept;
    container::String resolveTexturePath(const container::String& filename);
    const RawTexture* loadTGA(const container::String& path);
    const RawTexture* loadDDS(const container::String& path);

    // DDS decompression
    static void decompressDXT1(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);
    static void decompressDXT3(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);
    static void decompressDXT5(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height);

    // Texture pointers handed to render code must survive dense-map growth.
    container::HashMap<container::String, container::SharedPtr<RawTexture>> m_cache;
    container::HashMap<container::String, container::String> m_aliasCache;
    container::HashSet<container::String> m_missingTextures;
    container::SharedPtr<RawTexture> m_placeholder;
    uint64_t m_requests = 0;
    uint64_t m_cacheHits = 0;
    uint64_t m_cacheMisses = 0;
    uint64_t m_decodeAttempts = 0;
    uint64_t m_decodeSucceeded = 0;
    uint64_t m_decodeFailed = 0;
    uint64_t m_missingSources = 0;
    uint64_t m_unsupportedSources = 0;
    uint64_t m_resets = 0;
    uint64_t m_generation = 1;
};

} // namespace engine
