#include "DX12Renderer.h"

#include "UiSrvInvalidation.h"
#include "engine/renderer/d3d12/runtime/D3D12PerformanceSettings.h"
#include "engine/texture/TextureManager.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace engine {

uint32_t DX12Renderer::getOrCreateTextureSrv(const RawTexture* tex) {
    if (!tex || tex->rendererIdentity == 0) return UINT32_MAX;
    auto it = m_textureCache.find(tex->rendererIdentity);
    if (it != m_textureCache.end()) {
        ++m_uiTextureLifecycle.cacheHits;
        it->second.lastUsedFrame = m_uiSrvCacheFrame;
        return it->second.srvIndex;
    }
    ++m_uiTextureLifecycle.cacheMisses;
    if (!makeUiSrvCacheRoom(
            m_textureCache,
            d3d12::performance_limits::kUiTextureSrvCacheBudget)) {
        ++m_uiTextureLifecycle.failures;
        return UINT32_MAX;
    }

    const uint32_t index = m_d3d12.uploadTexture(
        tex->pixels.data(), tex->width, tex->height);
    if (index == UINT32_MAX) {
        ++m_uiTextureLifecycle.failures;
        return UINT32_MAX;
    }
    m_textureCache[tex->rendererIdentity] = {
        .srvIndex = index,
        .lastUsedFrame = m_uiSrvCacheFrame,
    };
    ++m_uiTextureLifecycle.published;
    return index;
}

uint32_t DX12Renderer::getOrCreateGlyphSrv(
    uint64_t glyphIdentity, const void* pixels, uint32_t width,
    uint32_t height) {
    if (glyphIdentity == 0) return UINT32_MAX;
    auto it = m_glyphCache.find(glyphIdentity);
    if (it != m_glyphCache.end()) {
        ++m_uiGlyphLifecycle.cacheHits;
        it->second.lastUsedFrame = m_uiSrvCacheFrame;
        return it->second.srvIndex;
    }
    ++m_uiGlyphLifecycle.cacheMisses;
    if (!makeUiSrvCacheRoom(
            m_glyphCache,
            d3d12::performance_limits::kUiGlyphSrvCacheBudget)) {
        ++m_uiGlyphLifecycle.failures;
        return UINT32_MAX;
    }
    const uint32_t index = m_d3d12.uploadTexture(pixels, width, height);
    if (index == UINT32_MAX) {
        ++m_uiGlyphLifecycle.failures;
        return UINT32_MAX;
    }
    m_glyphCache[glyphIdentity] = {
        .srvIndex = index,
        .lastUsedFrame = m_uiSrvCacheFrame,
    };
    ++m_uiGlyphLifecycle.published;
    return index;
}

bool DX12Renderer::makeUiSrvCacheRoom(
    container::HashMap<uint64_t, UiSrvCacheEntry>& cache,
    size_t budget) {
    if (budget == 0 ||
        m_uiSrvCacheFrame < m_uiSrvPressureBlockedUntilFrame) {
        return false;
    }
    // UI shares the heap with world textures and fixed renderer descriptors.
    // Preserve the explicit headroom before admitting a new cache entry;
    // eviction below cannot make a descriptor immediately reusable while its
    // fence is still in flight.
    const render::SrvDescriptorRenderStats srvStats =
        m_d3d12.srvDescriptorStats();
    if (srvStats.available <=
        d3d12::performance_limits::kSrvFixedDescriptorReserve) {
        return false;
    }
    if (cache.size() < budget) return true;

    const auto victim = std::min_element(
        cache.begin(), cache.end(), [](const auto& left, const auto& right) {
            return std::tie(left.second.lastUsedFrame, left.first) <
                std::tie(right.second.lastUsedFrame, right.first);
        });
    if (victim == cache.end()) return false;
    if (victim->second.srvIndex != UINT32_MAX) {
        m_d3d12.freeTexture(victim->second.srvIndex);
    }
    UiSrvCacheLifecycleCounters& lifecycle =
        &cache == &m_textureCache
        ? m_uiTextureLifecycle : m_uiGlyphLifecycle;
    ++lifecycle.evictions;
    cache.erase(victim);
    m_uiSrvPressureBlockedUntilFrame = m_uiSrvCacheFrame +
        d3d12::performance_limits::kUiSrvPressureRetirementFrames;
    if (m_uiSrvPressureBlockedUntilFrame < m_uiSrvCacheFrame) {
        m_uiSrvPressureBlockedUntilFrame = UINT64_MAX;
    }
    return false;
}

void DX12Renderer::processUiSrvInvalidations() {
    const container::Vector<render::UiSrvInvalidation> invalidations =
        render::takeUiSrvInvalidations();
    for (const render::UiSrvInvalidation& invalidation : invalidations) {
        auto& cache = invalidation.kind == render::UiSrvResourceKind::Texture
            ? m_textureCache : m_glyphCache;
        const auto found = cache.find(invalidation.identity);
        if (found == cache.end()) continue;
        if (found->second.srvIndex != UINT32_MAX) {
            m_d3d12.freeTexture(found->second.srvIndex);
        }
        UiSrvCacheLifecycleCounters& lifecycle =
            invalidation.kind == render::UiSrvResourceKind::Texture
            ? m_uiTextureLifecycle : m_uiGlyphLifecycle;
        ++lifecycle.evictions;
        cache.erase(found);
    }
    const uint64_t dropped = render::takeDroppedUiSrvInvalidationCount();
    if (dropped != 0) {
        TD_LOG_WARN(
            "[DX12Renderer] {} UI SRV invalidation notifications were dropped; idle retirement remains active",
            dropped);
    }
}

void DX12Renderer::pruneUiSrvCaches() {
    const uint64_t lifetime =
        d3d12::performance_limits::kUiSrvIdleLifetimeFrames;
    if (m_uiSrvCacheFrame <= lifetime) return;
    const uint64_t oldestLiveFrame = m_uiSrvCacheFrame - lifetime;

    const auto prune = [this, oldestLiveFrame](
        auto& cache, UiSrvCacheLifecycleCounters& lifecycle) {
        container::Vector<uint64_t> retired;
        retired.reserve(cache.size());
        for (const auto& [identity, entry] : cache) {
            if (entry.lastUsedFrame < oldestLiveFrame) {
                retired.push_back(identity);
            }
        }
        for (const uint64_t identity : retired) {
            const auto found = cache.find(identity);
            if (found == cache.end()) continue;
            if (found->second.srvIndex != UINT32_MAX) {
                m_d3d12.freeTexture(found->second.srvIndex);
            }
            cache.erase(found);
            ++lifecycle.evictions;
        }
    };
    prune(m_textureCache, m_uiTextureLifecycle);
    prune(m_glyphCache, m_uiGlyphLifecycle);
}

void DX12Renderer::releaseUiSrvCaches() {
    const auto release = [this](
        auto& cache, UiSrvCacheLifecycleCounters& lifecycle) {
        lifecycle.evictions += cache.size();
        ++lifecycle.resets;
        for (const auto& [identity, entry] : cache) {
            static_cast<void>(identity);
            if (entry.srvIndex != UINT32_MAX) {
                m_d3d12.freeTexture(entry.srvIndex);
            }
        }
        cache.clear();
    };
    release(m_textureCache, m_uiTextureLifecycle);
    release(m_glyphCache, m_uiGlyphLifecycle);
}

} // namespace engine
