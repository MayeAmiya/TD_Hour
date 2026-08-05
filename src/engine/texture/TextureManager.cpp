#include "core/container/container_types.h"
#include "TextureManager.h"
#include "MappedImageCollection.h"
#include "engine/renderer/runtime/RendererIdentity.h"
#include "engine/renderer/runtime/UiSrvInvalidation.h"
#include "data/tga/TgaLoader.h"
#include "data/dds/DdsLoader.h"
#include "data/dds/DdsTypes.h"
#include "LocaleResourceLocator.h"
#include "VFS.h"
#include "debug/debug.h"
#include "engine/texture/TextureDefaults.h"
#include "core/constants/Paths.h"
#include "core/constants/Strings.h"
#include "core/constants/Colors.h"
#include "core/constants/Widget.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace engine {

namespace {
std::atomic<uint64_t> g_nextTextureRendererIdentity{1};
}

uint64_t TextureManager::allocateRendererIdentity() noexcept {
    const uint64_t identity =
        render::allocateMonotonicRendererIdentity(
            g_nextTextureRendererIdentity);
#if TD_DEBUG_ENABLED
    if (identity == 0) {
        TD_LOG_ERROR(
            "[TextureManager] Renderer identity space exhausted; refusing a cacheable texture identity");
    }
#endif
    return identity;
}

TextureManager::TextureManager() {}
TextureManager::~TextureManager() { destroyAll(); }

const RawTexture* TextureManager::updateRuntimeRgbaTexture(
    const container::String& key, uint32_t width, uint32_t height,
    container::Vector<uint8_t> rgbaPixels) {
    if (key.empty() || width == 0 || height == 0 ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
            static_cast<uint64_t>(SIZE_MAX) / 4u ||
        rgbaPixels.size() != static_cast<size_t>(width) * height * 4u) {
        return nullptr;
    }
    const container::String cacheKey = "runtime:" + key;
    auto& slot = m_cache[cacheKey];
    if (slot && slot->rendererIdentity != 0) {
        render::publishUiSrvInvalidation(
            render::UiSrvResourceKind::Texture,
            slot->rendererIdentity);
    }
    slot = std::make_shared<RawTexture>();
    slot->pixels = std::move(rgbaPixels);
    slot->gpuPixels.clear();
    slot->mips.clear();
    slot->sourcePath = cacheKey;
    slot->rendererIdentity = allocateRendererIdentity();
    slot->width = width;
    slot->height = height;
    slot->gpuFormat = RawTextureGpuFormat::Rgba8;
    return slot->rendererIdentity != 0 ? slot.get() : nullptr;
}

void TextureManager::destroyAll() {
    ++m_resets;
    ++m_generation;
    if (m_generation == 0u) ++m_generation;
    for (const auto& [key, texture] : m_cache) {
        static_cast<void>(key);
        if (texture) {
            render::publishUiSrvInvalidation(
                render::UiSrvResourceKind::Texture,
                texture->rendererIdentity);
        }
    }
    if (m_placeholder) {
        render::publishUiSrvInvalidation(
            render::UiSrvResourceKind::Texture,
            m_placeholder->rendererIdentity);
    }
    m_cache.clear();
    m_aliasCache.clear();
    m_missingTextures.clear();
    m_placeholder.reset();
}

static container::String toLower(const container::String& s) {
    container::String r = s;
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

static container::String canonicalTexturePath(container::String value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.starts_with("./")) value.erase(0, 2);
    return toLower(value);
}

static bool buildRgbaMipChain(RawTexture& texture,
                              const container::Vector<uint8_t>& topLevel) {
    texture.gpuFormat = RawTextureGpuFormat::Rgba8;
    texture.gpuPixels.clear();
    texture.mips.clear();
    const uint64_t topLevelBytes = static_cast<uint64_t>(texture.width) *
        texture.height * 4u;
    if (texture.width == 0 || texture.height == 0 ||
        topLevelBytes > std::numeric_limits<size_t>::max() ||
        topLevel.size() != static_cast<size_t>(topLevelBytes)) {
        return false;
    }
    uint32_t width = texture.width;
    uint32_t height = texture.height;
    container::Vector<uint8_t> level = topLevel;
    while (width != 0 && height != 0) {
        const uint64_t rowPitch64 = static_cast<uint64_t>(width) * 4u;
        const uint64_t slicePitch64 = rowPitch64 * height;
        if (rowPitch64 > std::numeric_limits<uint32_t>::max() ||
            slicePitch64 > std::numeric_limits<uint32_t>::max() ||
            texture.gpuPixels.size() > std::numeric_limits<uint32_t>::max() ||
            level.size() != static_cast<size_t>(slicePitch64)) {
            texture.gpuPixels.clear();
            texture.mips.clear();
            return false;
        }
        const uint32_t rowPitch = static_cast<uint32_t>(rowPitch64);
        const uint32_t slicePitch = static_cast<uint32_t>(slicePitch64);
        texture.mips.push_back({
            .width = width,
            .height = height,
            .byteOffset = static_cast<uint32_t>(texture.gpuPixels.size()),
            .rowPitch = rowPitch,
            .slicePitch = slicePitch,
        });
        texture.gpuPixels.insert(texture.gpuPixels.end(), level.begin(), level.end());
        if (width == 1 && height == 1) break;

        const uint32_t nextWidth = std::max(1u, width / 2u);
        const uint32_t nextHeight = std::max(1u, height / 2u);
        container::Vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight * 4u);
        for (uint32_t y = 0; y < nextHeight; ++y) {
            for (uint32_t x = 0; x < nextWidth; ++x) {
                const uint32_t sourceXBegin =
                    static_cast<uint32_t>(static_cast<uint64_t>(x) * width / nextWidth);
                const uint32_t sourceXEnd =
                    static_cast<uint32_t>(static_cast<uint64_t>(x + 1u) * width / nextWidth);
                const uint32_t sourceYBegin =
                    static_cast<uint32_t>(static_cast<uint64_t>(y) * height / nextHeight);
                const uint32_t sourceYEnd =
                    static_cast<uint32_t>(static_cast<uint64_t>(y + 1u) * height / nextHeight);
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    uint32_t sum = 0;
                    uint32_t samples = 0;
                    for (uint32_t sampleY = sourceYBegin;
                         sampleY < sourceYEnd; ++sampleY) {
                        for (uint32_t sampleX = sourceXBegin;
                             sampleX < sourceXEnd; ++sampleX) {
                            sum += level[(static_cast<size_t>(sampleY) * width + sampleX) * 4u + channel];
                            ++samples;
                        }
                    }
                    next[(static_cast<size_t>(y) * nextWidth + x) * 4u + channel] =
                        static_cast<uint8_t>((sum + samples / 2u) / samples);
                }
            }
        }
        level = std::move(next);
        width = nextWidth;
        height = nextHeight;
    }
    return true;
}

static bool buildCompressedDdsMipChain(
    RawTexture& texture, const data::dds::DDSData& source) {
    RawTextureGpuFormat gpuFormat = RawTextureGpuFormat::Rgba8;
    uint32_t blockBytes = 0;
    switch (source.format) {
    case data::dds::DDSFormat::DXT1:
        gpuFormat = RawTextureGpuFormat::Bc1;
        blockBytes = 8;
        break;
    case data::dds::DDSFormat::DXT2:
    case data::dds::DDSFormat::DXT3:
        gpuFormat = RawTextureGpuFormat::Bc2;
        blockBytes = 16;
        break;
    case data::dds::DDSFormat::DXT4:
    case data::dds::DDSFormat::DXT5:
        gpuFormat = RawTextureGpuFormat::Bc3;
        blockBytes = 16;
        break;
    default:
        return false;
    }
    if (source.depth != 1 || source.width == 0 || source.height == 0) return false;

    uint32_t maximumMipLevels = 1;
    for (uint32_t extent = std::max(source.width, source.height);
         extent > 1; extent >>= 1) {
        ++maximumMipLevels;
    }
    const uint32_t mipLevels = std::max(1u, source.mipLevels);
    if (mipLevels > maximumMipLevels) return false;

    container::Vector<RawTextureMip> mips;
    mips.reserve(mipLevels);
    uint32_t width = source.width;
    uint32_t height = source.height;
    uint64_t offset = 0;
    for (uint32_t level = 0; level < mipLevels; ++level) {
        const uint64_t blocksWide = width / 4u + (width % 4u != 0 ? 1u : 0u);
        const uint64_t blocksHigh = height / 4u + (height % 4u != 0 ? 1u : 0u);
        const uint64_t rowPitch = blocksWide * blockBytes;
        const uint64_t slicePitch = rowPitch * blocksHigh;
        if (rowPitch > std::numeric_limits<uint32_t>::max() ||
            slicePitch > std::numeric_limits<uint32_t>::max() ||
            offset > std::numeric_limits<uint32_t>::max() ||
            slicePitch > source.pixels.size() ||
            offset > source.pixels.size() - slicePitch) {
            return false;
        }
        mips.push_back({
            .width = width,
            .height = height,
            .byteOffset = static_cast<uint32_t>(offset),
            .rowPitch = static_cast<uint32_t>(rowPitch),
            .slicePitch = static_cast<uint32_t>(slicePitch),
        });
        offset += slicePitch;
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
    }

    container::Vector<uint8_t> gpuPixels = source.pixels;
    texture.gpuFormat = gpuFormat;
    texture.gpuPixels = std::move(gpuPixels);
    texture.mips = std::move(mips);
    return true;
}

container::String TextureManager::resolveTexturePath(const container::String& filename) {
    const auto locator = io::acquireLocaleResourceLocator();
    if (locator) {
        const std::optional<container::String> resolved = locator->resolve(
            io::LocaleResourceKind::Texture, filename);
        return resolved ? *resolved : container::String{};
    }
    // Standalone texture diagnostics retain exact VFS paths, but never scan
    // by basename or substring when no launch locator has been published.
    return io::VFS::instance().exists(filename)
        ? filename : container::String{};
}

container::String TextureManager::resolveTextureIdentity(const container::String& filename) {
    const container::String resolved = resolveTexturePath(filename);
    return canonicalTexturePath(resolved.empty() ? filename : resolved);
}

const RawTexture* TextureManager::loadTexture(const container::String& filename) {
    ++m_requests;
    auto it = m_cache.find(filename);
    if (it != m_cache.end()) {
        ++m_cacheHits;
        return it->second.get();
    }

    auto aliasIt = m_aliasCache.find(filename);
    if (aliasIt != m_aliasCache.end()) {
        auto resolvedIt = m_cache.find(aliasIt->second);
        if (resolvedIt != m_cache.end()) {
            ++m_cacheHits;
            return resolvedIt->second.get();
        }
    }

    const auto locator = io::acquireLocaleResourceLocator();
    container::Vector<container::String> candidates;
    if (locator) {
        candidates = locator->candidates(
            io::LocaleResourceKind::Texture, filename);
    } else if (io::VFS::instance().exists(filename)) {
        candidates.push_back(filename);
    }
    if (candidates.empty()) {
        ++m_cacheMisses;
        ++m_missingSources;
        if (m_missingTextures.insert(filename).second) {
            TD_LOG_DEBUG(
                "[TextureManager] Optional file not found: {} (using placeholder)",
                filename);
        }
        return getPlaceholder();
    }

    ++m_cacheMisses;

    for (const container::String& fullPath : candidates) {
        if (locator && !locator->contains(fullPath)) continue;
        auto resolvedIt = m_cache.find(fullPath);
        if (resolvedIt != m_cache.end()) {
            ++m_cacheHits;
            m_aliasCache[filename] = fullPath;
            return resolvedIt->second.get();
        }

        const container::String resolvedLower = toLower(fullPath);
        const RawTexture* tex = nullptr;
        if (resolvedLower.size() >= 4 &&
            resolvedLower.substr(resolvedLower.size() - 4) ==
                EXT_DDS.data()) {
            ++m_decodeAttempts;
            tex = loadDDS(fullPath);
        } else if (resolvedLower.size() >= 4 &&
                   resolvedLower.substr(resolvedLower.size() - 4) ==
                       EXT_TGA.data()) {
            ++m_decodeAttempts;
            tex = loadTGA(fullPath);
        } else {
            ++m_unsupportedSources;
            TD_LOG_WARN("[TextureManager] Unsupported format: {}", fullPath);
            continue;
        }

        if (tex) {
            ++m_decodeSucceeded;
            m_aliasCache[filename] = fullPath;
            return tex;
        }
        ++m_decodeFailed;
    }
    return getPlaceholder();
}

MappedImageResult TextureManager::findMappedImage(const container::String& name) {
    MappedImageResult result;

    auto* img = MappedImageCollection::instance().findByName(name);
    if (!img) return result;

    // Load the texture
    const RawTexture* tex = loadTexture(img->textureFile);

    // Fallback for known textures with missing files
    if (!tex || tex == m_placeholder.get()) {
        if (name == "MainMenuBackdrop") {
            tex = getOrCreateSolid("MainMenuBackdrop_fallback", COLOR_MAINMENU_BACKDROP_FALLBACK);
        }
    }

    result.texture = tex;
    result.left = img->left;
    result.top = img->top;
    result.right = img->right;
    result.bottom = img->bottom;
    result.texW = img->textureWidth;
    result.texH = img->textureHeight;
    result.found = true;

    return result;
}

const RawTexture* TextureManager::loadTGA(const container::String& path) {
    data::tga::TgaLoader loader;
    if (!loader.loadFromFile(path)) {
        TD_LOG_WARN("[TextureManager] TGA load failed: {} - {}", path, loader.error());
        return nullptr;
    }

    const auto& result = loader.result();
    if (result.format == data::tga::TGAFormat::Unknown) {
        TD_LOG_WARN("[TextureManager] TGA unknown format: {}", path);
        return nullptr;
    }

    const uint64_t rgbaByteCount = static_cast<uint64_t>(result.width) *
        result.height * 4u;
    if (rgbaByteCount == 0 || rgbaByteCount > std::numeric_limits<size_t>::max()) {
        TD_LOG_WARN("[TextureManager] TGA dimensions overflow RGBA storage: {}", path);
        return nullptr;
    }
    container::Vector<uint8_t> rgba(static_cast<size_t>(rgbaByteCount));
    uint32_t srcBPP = (result.format == data::tga::TGAFormat::A8R8G8B8) ? 4 :
                      (result.format == data::tga::TGAFormat::R8G8B8) ? 3 : 1;

    for (uint32_t i = 0; i < result.width * result.height; ++i) {
        const uint8_t* src = result.pixels.data() + i * srcBPP;
        uint8_t* dst = rgba.data() + i * 4;

        if (result.format == data::tga::TGAFormat::L8) {
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = 255;
        } else {
            dst[0] = src[0]; // R
            dst[1] = src[1]; // G
            dst[2] = src[2]; // B
            dst[3] = (result.format == data::tga::TGAFormat::A8R8G8B8) ? src[3] : 255;
        }
    }

    // Store as RawTexture
    auto& texture = m_cache[path];
    if (!texture) {
        texture = std::make_shared<RawTexture>();
        texture->rendererIdentity = allocateRendererIdentity();
    }
    RawTexture& tex = *texture;
    tex.width = result.width;
    tex.height = result.height;
    tex.sourcePath = canonicalTexturePath(path);
    tex.pixels = std::move(rgba);
    if (!buildRgbaMipChain(tex, tex.pixels)) {
        m_cache.erase(path);
        TD_LOG_WARN("[TextureManager] TGA mip generation failed: {}", path);
        return nullptr;
    }
    return &tex;
}

const RawTexture* TextureManager::loadDDS(const container::String& path) {
    data::dds::DdsLoader loader;
    if (!loader.loadFromFile(path)) {
        TD_LOG_WARN("[TextureManager] DDS load failed: {} - {}", path, loader.error());
        return nullptr;
    }

    const auto& result = loader.result();
    if (result.format == data::dds::DDSFormat::Unknown) {
        TD_LOG_WARN("[TextureManager] DDS unknown format: {}", path);
        return nullptr;
    }

    uint32_t w = result.width;
    uint32_t h = result.height;
    const uint64_t pixelCount = static_cast<uint64_t>(w) * h;
    if (w == 0 || h == 0 ||
        pixelCount > std::numeric_limits<uint64_t>::max() / 4u) {
        TD_LOG_WARN("[TextureManager] DDS dimensions overflow RGBA storage: {}", path);
        return nullptr;
    }
    const uint64_t rgbaByteCount = pixelCount * 4u;
    if (rgbaByteCount > std::numeric_limits<size_t>::max()) {
        TD_LOG_WARN("[TextureManager] DDS dimensions exceed addressable RGBA storage: {}", path);
        return nullptr;
    }

    uint64_t sourceTopLevelBytes = 0;
    switch (result.format) {
    case data::dds::DDSFormat::A8R8G8B8:
    case data::dds::DDSFormat::X8R8G8B8:
        sourceTopLevelBytes = rgbaByteCount;
        break;
    case data::dds::DDSFormat::R8G8B8:
        sourceTopLevelBytes = pixelCount * 3u;
        break;
    case data::dds::DDSFormat::R5G6B5:
        sourceTopLevelBytes = pixelCount * 2u;
        break;
    case data::dds::DDSFormat::DXT1:
    case data::dds::DDSFormat::DXT2:
    case data::dds::DDSFormat::DXT3:
    case data::dds::DDSFormat::DXT4:
    case data::dds::DDSFormat::DXT5: {
        const uint64_t blocksWide = w / 4u + (w % 4u != 0 ? 1u : 0u);
        const uint64_t blocksHigh = h / 4u + (h % 4u != 0 ? 1u : 0u);
        const uint64_t blockBytes = result.format == data::dds::DDSFormat::DXT1
            ? 8u : 16u;
        const uint64_t blockCount = blocksWide * blocksHigh;
        if (blockCount > std::numeric_limits<uint64_t>::max() / blockBytes) {
            TD_LOG_WARN("[TextureManager] DDS block layout overflows storage: {}", path);
            return nullptr;
        }
        sourceTopLevelBytes = blockCount * blockBytes;
        break;
    }
    default:
        TD_LOG_WARN("[TextureManager] DDS format not supported: {}", path);
        return nullptr;
    }
    if (sourceTopLevelBytes == 0 || sourceTopLevelBytes > result.pixels.size()) {
        TD_LOG_WARN("[TextureManager] DDS top-level payload is truncated: {}", path);
        return nullptr;
    }

    container::Vector<uint8_t> rgba(static_cast<size_t>(rgbaByteCount));

    switch (result.format) {
    case data::dds::DDSFormat::A8R8G8B8:
    case data::dds::DDSFormat::X8R8G8B8: {
        for (size_t i = 0; i < static_cast<size_t>(pixelCount); ++i) {
            const uint8_t* src = result.pixels.data() + i * 4;
            uint8_t* dst = rgba.data() + i * 4;
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
            dst[3] = (result.format == data::dds::DDSFormat::A8R8G8B8) ? src[3] : 255;
        }
        break;
    }
    case data::dds::DDSFormat::R8G8B8: {
        for (size_t i = 0; i < static_cast<size_t>(pixelCount); ++i) {
            const uint8_t* src = result.pixels.data() + i * 3;
            uint8_t* dst = rgba.data() + i * 4;
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        }
        break;
    }
    case data::dds::DDSFormat::R5G6B5: {
        for (size_t i = 0; i < static_cast<size_t>(pixelCount); ++i) {
            uint16_t pixel;
            std::memcpy(&pixel, result.pixels.data() + i * 2, 2);
            uint8_t* dst = rgba.data() + i * 4;
            dst[0] = static_cast<uint8_t>((pixel >> 11) << 3);
            dst[1] = static_cast<uint8_t>(((pixel >> 5) & 0x3F) << 2);
            dst[2] = static_cast<uint8_t>((pixel & 0x1F) << 3);
            dst[3] = 255;
        }
        break;
    }
    case data::dds::DDSFormat::DXT1:
        decompressDXT1(result.pixels.data(), rgba.data(), w, h);
        break;
    case data::dds::DDSFormat::DXT2:
    case data::dds::DDSFormat::DXT3:
        decompressDXT3(result.pixels.data(), rgba.data(), w, h);
        break;
    case data::dds::DDSFormat::DXT4:
    case data::dds::DDSFormat::DXT5:
        decompressDXT5(result.pixels.data(), rgba.data(), w, h);
        break;
    default:
        return nullptr;
    }

    auto& texture = m_cache[path];
    if (!texture) {
        texture = std::make_shared<RawTexture>();
        texture->rendererIdentity = allocateRendererIdentity();
    }
    RawTexture& tex = *texture;
    tex.width = w;
    tex.height = h;
    tex.sourcePath = canonicalTexturePath(path);
    tex.pixels = std::move(rgba);
    if (!buildCompressedDdsMipChain(tex, result) &&
        !buildRgbaMipChain(tex, tex.pixels)) {
        m_cache.erase(path);
        TD_LOG_WARN("[TextureManager] DDS mip preparation failed: {}", path);
        return nullptr;
    }
    return &tex;
}

// ── DXT decompression (unchanged) ────────────────────────────────────────

static inline void color565ToRGB(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = static_cast<uint8_t>((c >> 11) << 3);
    g = static_cast<uint8_t>(((c >> 5) & 0x3F) << 2);
    b = static_cast<uint8_t>((c & 0x1F) << 3);
}

static inline void buildDXTColorTable(uint16_t color0, uint16_t color1, bool allowOneBitAlpha, uint8_t table[4][3]) {
    uint8_t r0, g0, b0, r1, g1, b1;
    color565ToRGB(color0, r0, g0, b0);
    color565ToRGB(color1, r1, g1, b1);

    table[0][0] = r0; table[0][1] = g0; table[0][2] = b0;
    table[1][0] = r1; table[1][1] = g1; table[1][2] = b1;

    if (color0 > color1 || !allowOneBitAlpha) {
        table[2][0] = static_cast<uint8_t>((2 * r0 + r1 + 1) / 3);
        table[2][1] = static_cast<uint8_t>((2 * g0 + g1 + 1) / 3);
        table[2][2] = static_cast<uint8_t>((2 * b0 + b1 + 1) / 3);
        table[3][0] = static_cast<uint8_t>((r0 + 2 * r1 + 1) / 3);
        table[3][1] = static_cast<uint8_t>((g0 + 2 * g1 + 1) / 3);
        table[3][2] = static_cast<uint8_t>((b0 + 2 * b1 + 1) / 3);
    } else {
        table[2][0] = static_cast<uint8_t>((r0 + r1) / 2);
        table[2][1] = static_cast<uint8_t>((g0 + g1) / 2);
        table[2][2] = static_cast<uint8_t>((b0 + b1) / 2);
        table[3][0] = 0;
        table[3][1] = 0;
        table[3][2] = 0;
    }
}

void TextureManager::decompressDXT1(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    uint32_t blocksX = width / 4u + (width % 4u != 0 ? 1u : 0u);
    uint32_t blocksY = height / 4u + (height % 4u != 0 ? 1u : 0u);

    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            uint16_t color0, color1;
            std::memcpy(&color0, src, 2); src += 2;
            std::memcpy(&color1, src, 2); src += 2;
            uint32_t indices;
            std::memcpy(&indices, src, 4); src += 4;

            uint8_t colors[4][3];
            buildDXTColorTable(color0, color1, true, colors);

            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    uint32_t px = bx * 4 + x;
                    uint32_t py = by * 4 + y;
                    if (px >= width || py >= height) continue;

                    uint32_t idx = (indices >> (2 * (4 * y + x))) & 3;
                    uint8_t* out = dst +
                        (static_cast<size_t>(py) * width + px) * 4u;

                    out[0] = colors[idx][0];
                    out[1] = colors[idx][1];
                    out[2] = colors[idx][2];
                    if (color0 <= color1 && idx == 3) {
                        out[3] = 0;
                    } else {
                        out[3] = 255;
                    }
                }
            }
        }
    }
}

void TextureManager::decompressDXT3(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    uint32_t blocksX = width / 4u + (width % 4u != 0 ? 1u : 0u);
    uint32_t blocksY = height / 4u + (height % 4u != 0 ? 1u : 0u);

    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            uint64_t alphaBlock;
            std::memcpy(&alphaBlock, src, 8); src += 8;

            uint16_t color0, color1;
            std::memcpy(&color0, src, 2); src += 2;
            std::memcpy(&color1, src, 2); src += 2;
            uint32_t indices;
            std::memcpy(&indices, src, 4); src += 4;

            uint8_t colors[4][3];
            buildDXTColorTable(color0, color1, false, colors);

            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    uint32_t px = bx * 4 + x;
                    uint32_t py = by * 4 + y;
                    if (px >= width || py >= height) continue;

                    uint32_t idx = (indices >> (2 * (4 * y + x))) & 3;
                    uint8_t alpha = static_cast<uint8_t>(((alphaBlock >> (4 * (4 * y + x))) & 0xF) * 17);

                    uint8_t* out = dst +
                        (static_cast<size_t>(py) * width + px) * 4u;
                    out[0] = colors[idx][0];
                    out[1] = colors[idx][1];
                    out[2] = colors[idx][2];
                    out[3] = alpha;
                }
            }
        }
    }
}

void TextureManager::decompressDXT5(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    uint32_t blocksX = width / 4u + (width % 4u != 0 ? 1u : 0u);
    uint32_t blocksY = height / 4u + (height % 4u != 0 ? 1u : 0u);

    for (uint32_t by = 0; by < blocksY; ++by) {
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            uint8_t alpha0 = src[0];
            uint8_t alpha1 = src[1];
            uint64_t alphaBits = 0;
            std::memcpy(&alphaBits, src + 2, 6);
            src += 8;

            uint16_t color0, color1;
            std::memcpy(&color0, src, 2); src += 2;
            std::memcpy(&color1, src, 2); src += 2;
            uint32_t indices;
            std::memcpy(&indices, src, 4); src += 4;

            uint8_t colors[4][3];
            buildDXTColorTable(color0, color1, false, colors);

            uint8_t alphas[8];
            alphas[0] = alpha0;
            alphas[1] = alpha1;
            if (alpha0 > alpha1) {
                alphas[2] = static_cast<uint8_t>((6 * alpha0 + 1 * alpha1 + 3) / 7);
                alphas[3] = static_cast<uint8_t>((5 * alpha0 + 2 * alpha1 + 3) / 7);
                alphas[4] = static_cast<uint8_t>((4 * alpha0 + 3 * alpha1 + 3) / 7);
                alphas[5] = static_cast<uint8_t>((3 * alpha0 + 4 * alpha1 + 3) / 7);
                alphas[6] = static_cast<uint8_t>((2 * alpha0 + 5 * alpha1 + 3) / 7);
                alphas[7] = static_cast<uint8_t>((1 * alpha0 + 6 * alpha1 + 3) / 7);
            } else {
                alphas[2] = static_cast<uint8_t>((4 * alpha0 + 1 * alpha1 + 2) / 5);
                alphas[3] = static_cast<uint8_t>((3 * alpha0 + 2 * alpha1 + 2) / 5);
                alphas[4] = static_cast<uint8_t>((2 * alpha0 + 3 * alpha1 + 2) / 5);
                alphas[5] = static_cast<uint8_t>((1 * alpha0 + 4 * alpha1 + 2) / 5);
                alphas[6] = 0;
                alphas[7] = 255;
            }

            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    uint32_t px = bx * 4 + x;
                    uint32_t py = by * 4 + y;
                    if (px >= width || py >= height) continue;

                    uint32_t idx = (indices >> (2 * (4 * y + x))) & 3;
                    uint32_t aidx = 3 * (4 * y + x);
                    uint8_t alphaIdx = static_cast<uint8_t>((alphaBits >> aidx) & 7);

                    uint8_t* out = dst +
                        (static_cast<size_t>(py) * width + px) * 4u;
                    out[0] = colors[idx][0];
                    out[1] = colors[idx][1];
                    out[2] = colors[idx][2];
                    out[3] = alphas[alphaIdx];
                }
            }
        }
    }
}

// ── Placeholder / solid ──────────────────────────────────────────────────

const RawTexture* TextureManager::getOrCreateSolid(const container::String& key, uint32_t argbColor) {
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.get();

    uint8_t r = static_cast<uint8_t>((argbColor >> 16) & 0xFF);
    uint8_t g = static_cast<uint8_t>((argbColor >> 8) & 0xFF);
    uint8_t b = static_cast<uint8_t>(argbColor & 0xFF);
    uint8_t a = static_cast<uint8_t>((argbColor >> 24) & 0xFF);

    auto& texture = m_cache[key];
    if (!texture) {
        texture = std::make_shared<RawTexture>();
        texture->rendererIdentity = allocateRendererIdentity();
    }
    RawTexture& tex = *texture;
    tex.pixels = { r, g, b, a };
    tex.width = 1;
    tex.height = 1;
    tex.sourcePath = canonicalTexturePath(key);
    (void)buildRgbaMipChain(tex, tex.pixels);
    return &tex;
}

const RawTexture* TextureManager::getPlaceholder() {
    if (m_placeholder && m_placeholder->hasData()) return m_placeholder.get();

    m_placeholder = std::make_shared<RawTexture>();

    m_placeholder->rendererIdentity = allocateRendererIdentity();

    constexpr int SIZE = engine::texture_defaults::PLACEHOLDER_TILE_SIZE;
    m_placeholder->pixels.resize(SIZE * SIZE * 4);

    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            uint8_t* px = m_placeholder->pixels.data() + (y * SIZE + x) * 4;
            bool black = ((x / 4) + (y / 4)) % 2 == 0;
            if (black) {
                px[0] = 0; px[1] = 0; px[2] = 0; px[3] = 255;
            } else {
                px[0] = 255; px[1] = 0; px[2] = 255; px[3] = 255;
            }
        }
    }
    m_placeholder->width = SIZE;
    m_placeholder->height = SIZE;
    m_placeholder->sourcePath = "__placeholder";
    (void)buildRgbaMipChain(*m_placeholder, m_placeholder->pixels);

    TD_LOG_INFO("[TextureManager] Created placeholder (pink/black checkerboard)");
    return m_placeholder.get();
}

const RawTexture* TextureManager::getCached(const container::String& name) const {
    auto it = m_cache.find(name);
    return (it != m_cache.end()) ? it->second.get() : nullptr;
}

TextureManagerStats TextureManager::stats() const noexcept {
    TextureManagerStats result{
        .aliases = m_aliasCache.size(),
        .negativeLookups = m_missingTextures.size(),
        .requests = m_requests,
        .cacheHits = m_cacheHits,
        .cacheMisses = m_cacheMisses,
        .decodeAttempts = m_decodeAttempts,
        .decodeSucceeded = m_decodeSucceeded,
        .decodeFailed = m_decodeFailed,
        .missingSources = m_missingSources,
        .unsupportedSources = m_unsupportedSources,
        .resets = m_resets,
        .generation = m_generation,
    };
    const auto addTextureBytes = [&](const RawTexture& texture) noexcept {
        result.cpuBytes += texture.pixels.size();
        result.cpuBytes += texture.gpuPixels.size();
        result.cpuBytes += texture.mips.size() * sizeof(RawTextureMip);
    };
    for (const auto& [key, texture] : m_cache) {
        static_cast<void>(key);
        if (!texture) continue;
        const bool fileDecoded = texture->sourcePath.ends_with(".dds") ||
            texture->sourcePath.ends_with(".tga");
        if (fileDecoded) ++result.decodedSources;
        else ++result.proceduralSources;
        addTextureBytes(*texture);
    }
    if (m_placeholder &&
        (m_placeholder->hasData() || m_placeholder->hasGpuData())) {
        addTextureBytes(*m_placeholder);
    }
    return result;
}

} // namespace engine
