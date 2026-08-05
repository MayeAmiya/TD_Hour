#include "engine/renderer/world/resource/WorldTextureDecodeWork.h"
#include "engine/renderer/world/resource/WorldTextureIdentity.h"
#include "engine/renderer/world/resource/WorldTextureCache.h"

#include "core/platform/runtime_threads.h"
#include "engine/renderer/d3d12/runtime/D3D12QualitySettings.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace engine::render::detail {
namespace {

using Lookup = WorldTextureDecodeService::Lookup;
using VariantKind = WorldTextureVariantKind;
using Request = WorldTextureDecodeRequest;
using Job = WorldTextureDecodeJob;
using Completion = WorldTextureDecodeCompletion;

[[nodiscard]] DXGI_FORMAT gpuFormat(RawTextureGpuFormat format,
                                    WorldTextureVariant variant) noexcept {
    // The original DX8/ZH world pipeline sampled authored colour maps as
    // ordinary UNORM values and combined them in gamma space. The modern
    // world shader deliberately preserves that presentation contract instead
    // of silently decoding colour SRVs to linear values.
    (void)variant;
    switch (format) {
    case RawTextureGpuFormat::Rgba8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RawTextureGpuFormat::Bc1:
        return DXGI_FORMAT_BC1_UNORM;
    case RawTextureGpuFormat::Bc2:
        return DXGI_FORMAT_BC2_UNORM;
    case RawTextureGpuFormat::Bc3:
        return DXGI_FORMAT_BC3_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] TextureManagerStats textureStatsDelta(
    const TextureManagerStats& after,
    const TextureManagerStats& before) noexcept {
    const auto delta = [](auto current, auto previous) {
        return current >= previous ? current - previous : current;
    };
    return {
        .decodedSources = delta(
            after.decodedSources, before.decodedSources),
        .proceduralSources = delta(
            after.proceduralSources, before.proceduralSources),
        .aliases = delta(after.aliases, before.aliases),
        .negativeLookups = delta(
            after.negativeLookups, before.negativeLookups),
        .cpuBytes = delta(after.cpuBytes, before.cpuBytes),
        .requests = delta(after.requests, before.requests),
        .cacheHits = delta(after.cacheHits, before.cacheHits),
        .cacheMisses = delta(after.cacheMisses, before.cacheMisses),
        .decodeAttempts = delta(
            after.decodeAttempts, before.decodeAttempts),
        .decodeSucceeded = delta(
            after.decodeSucceeded, before.decodeSucceeded),
        .decodeFailed = delta(
            after.decodeFailed, before.decodeFailed),
        .missingSources = delta(
            after.missingSources, before.missingSources),
        .unsupportedSources = delta(
            after.unsupportedSources, before.unsupportedSources),
        .resets = delta(after.resets, before.resets),
        .generation = after.generation,
    };
}

container::SharedPtr<const Lookup::Payload> buildPayload(
    const Request& request,
    const container::SharedPtr<const RawTexture>& source,
    container::String& diagnostic) {
    if (!source) {
        diagnostic = "decoded texture source unavailable";
        return {};
    }
    auto payload = std::make_shared<Lookup::Payload>();
    payload->sourceWidth = source->width;
    payload->sourceHeight = source->height;

    if (request.kind == VariantKind::Ordinary) {
        const d3d12::ReducedTextureMipRange range =
            d3d12::reducedTextureMipRange(
                source->width, source->height, source->mips.size(),
                request.reduction);
        if (range.mipCount == 0u) {
            diagnostic = "invalid ordinary texture mip range";
            return {};
        }
        payload->pixels =
            container::SharedPtr<const container::Vector<uint8_t>>(
                source, &source->gpuPixels);
        payload->format = gpuFormat(source->gpuFormat, request.variant);
        payload->width = range.width;
        payload->height = range.height;
        const size_t last = range.firstMip + range.mipCount;
        payload->mips.reserve(range.mipCount);
        for (size_t index = range.firstMip; index < last; ++index) {
            const RawTextureMip& mip = source->mips[index];
            if (mip.byteOffset > source->gpuPixels.size() ||
                mip.slicePitch > source->gpuPixels.size() - mip.byteOffset) {
                diagnostic = "malformed ordinary texture mip payload";
                return {};
            }
            payload->mips.push_back({mip.byteOffset, mip.rowPitch,
                                     mip.slicePitch});
            payload->byteSize += mip.slicePitch;
        }
    } else if (request.kind == VariantKind::TerrainColor) {
        std::optional<WorldTextureCache::TerrainColorMipPayload> terrain =
            WorldTextureCache::buildTerrainColorMipPayload(
                source->pixels, source->width, source->height,
                request.gridWidth);
        if (!terrain) {
            diagnostic = "texture source is incompatible with terrain grid";
            return {};
        }
        const d3d12::ReducedTextureMipRange range =
            d3d12::terrainColorTextureMipRange(
                source->width, source->height, terrain->mips.size(),
                request.reduction);
        if (range.mipCount == 0u) {
            diagnostic = "invalid terrain texture mip range";
            return {};
        }
        payload->pixels = std::make_shared<const container::Vector<uint8_t>>(
            std::move(terrain->rgbaPixels));
        // TerrainTextureClass copied authored RGB bytes into an ordinary DX8
        // colour atlas and the fixed-function terrain stages combined those
        // values in gamma space.  The reconstructed world target follows the
        // same legacy contract as ordinary W3D colour textures; exposing this
        // atlas through an SRGB view linearizes it once too many and makes the
        // complete terrain markedly darker than objects using UNORM views.
        payload->format = DXGI_FORMAT_R8G8B8A8_UNORM;
        payload->width = range.width;
        payload->height = range.height;
        const size_t last = range.firstMip + range.mipCount;
        payload->mips.reserve(range.mipCount);
        for (size_t index = range.firstMip; index < last; ++index) {
            const WorldTextureCache::TerrainColorMip& mip =
                terrain->mips[index];
            if (mip.byteOffset > payload->pixels->size() ||
                mip.slicePitch > payload->pixels->size() - mip.byteOffset) {
                diagnostic = "malformed terrain texture mip payload";
                return {};
            }
            payload->mips.push_back({mip.byteOffset, mip.rowPitch,
                                     mip.slicePitch});
            payload->byteSize += mip.slicePitch;
        }
    } else {
        std::optional<container::Vector<uint8_t>> alpha =
            WorldTextureCache::buildTerrainAlphaEdgePixels(
                source->pixels, source->width, source->height);
        if (!alpha) {
            diagnostic = "invalid terrain alpha-edge payload";
            return {};
        }
        const uint64_t rowPitch = static_cast<uint64_t>(source->width) * 4u;
        if (rowPitch > std::numeric_limits<uint32_t>::max() ||
            alpha->size() > std::numeric_limits<uint32_t>::max()) {
            diagnostic = "terrain alpha-edge payload exceeds upload limits";
            return {};
        }
        payload->pixels = std::make_shared<const container::Vector<uint8_t>>(
            std::move(*alpha));
        payload->format = DXGI_FORMAT_R8G8B8A8_UNORM;
        payload->width = source->width;
        payload->height = source->height;
        payload->mips.push_back({0u, static_cast<uint32_t>(rowPitch),
                                 static_cast<uint32_t>(payload->pixels->size())});
        payload->byteSize = payload->pixels->size();
    }
    return payload;
}

} // namespace

WorldTextureDecodeCompletion WorldTextureDecodeWorker::run(
    WorldTextureDecodeJob job) noexcept {
    platform::runtime::ThreadRoleScope role(
        platform::runtime::ThreadRole::Resource);
    Completion completion;
    completion.kind = job.kind;
    completion.request = std::move(job.request);
    completion.generation = job.generation;
    const auto started = std::chrono::steady_clock::now();
    try {
        if (job.kind == Job::Kind::DecodeSource) {
            if (!job.decoder) {
                completion.diagnostic = "texture decoder unavailable";
                return completion;
            }
            completion.decoder = job.decoder;
            const TextureManagerStats statsBefore = job.decoder->stats();
            const RawTexture* fallback = job.decoder->getPlaceholder();
            const RawTexture* decoded =
                job.decoder->loadTexture(completion.request.logicalName);
            if (decoded && decoded != fallback && decoded->hasData() &&
                decoded->hasGpuData() && decoded->width != 0u &&
                decoded->height != 0u) {
                completion.canonicalSourceKey =
                    canonicalWorldTextureIdentity(decoded->sourcePath);
                completion.source = std::make_shared<RawTexture>(*decoded);
                completion.payload = buildPayload(
                    completion.request, completion.source,
                    completion.diagnostic);
            } else {
                completion.diagnostic =
                    "texture source missing or undecodable";
            }
            completion.stats = textureStatsDelta(
                job.decoder->stats(), statsBefore);
        } else {
            completion.payload = buildPayload(completion.request, job.source,
                                              completion.diagnostic);
        }
    } catch (const std::exception& exception) {
        completion.diagnostic = exception.what();
    } catch (...) {
        completion.diagnostic = "unknown texture CPU preparation failure";
    }
    completion.workerNanoseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    return completion;
}


} // namespace engine::render::detail
