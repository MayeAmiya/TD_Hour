#include "engine/renderer/world/terrain/TerrainBibUpdateOwner.h"

#include "engine/renderer/world/resource/WorldTextureCache.h"
#include "engine/renderer/world/terrain/TerrainGpuGeometryOwner.h"
#include "engine/renderer/world/terrain/TerrainGpuMaterialOwner.h"
#include "engine/renderer/world/terrain/TerrainSceneFuture.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <utility>

namespace engine::render::detail {
namespace {

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

uint64_t contentHash(container::Span<const TerrainBibRenderData> bibs) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) noexcept {
        constexpr uint64_t prime = 1099511628211ull;
        for (uint32_t byte = 0; byte < 8u; ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8u));
            hash *= prime;
        }
    };
    mix(bibs.size());
    for (const TerrainBibRenderData& bib : bibs) {
        mix(bib.ownerObjectId);
        mix(static_cast<uint8_t>(bib.kind));
        mix(bib.red);
        mix(static_cast<uint8_t>(bib.tint));
        mix(bib.receivesVisibility);
        for (char value : bib.textureName) {
            mix(static_cast<uint8_t>(value));
        }
        for (const RenderVector& corner : bib.corners) {
            mix(std::bit_cast<uint32_t>(corner.x()));
            mix(std::bit_cast<uint32_t>(corner.y()));
            mix(std::bit_cast<uint32_t>(corner.z()));
        }
    }
    return hash;
}

container::StringView textureName(const TerrainBibRenderData& source) {
    return source.textureName.empty()
        ? container::StringView{"TBBib.tga"}
        : container::StringView{source.textureName};
}

} // namespace

bool TerrainBibUpdateOwner::CpuCandidate::ready() const noexcept {
    return std::all_of(
        tasks.begin(), tasks.end(),
        [](const std::future<std::optional<TerrainBibMeshCpu>>& task) {
            return !task.valid() ||
                task.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready;
        });
}

void TerrainBibUpdateOwner::CpuCandidate::requestCancel() noexcept {
    cancelRequested->store(true, std::memory_order_release);
}

TerrainBibUpdateOwner::~TerrainBibUpdateOwner() {
    requestCancel();
}

bool TerrainBibUpdateOwner::update(
    container::Span<const TerrainBibRenderData> bibs,
    WorldTextureCache* textures,
    TerrainGpuMaterialOwner& materials,
    TerrainGpuGeometryOwner& geometry,
    container::String* error) {
    const uint64_t hash = contentHash(bibs);
    m_requestedContentHash = hash;

    if (textures) {
        for (const TerrainBibRenderData& source : bibs) {
            if (!textures->prepare(
                    textureName(source),
                    WorldTextureCache::Variant::ColorLegacyGamma,
                    RenderAssetPriority::Visible)) {
                return true;
            }
        }
    }

    if (m_candidate) {
        CpuCandidate& candidate = *m_candidate;
        const bool stale = candidate.contentHash != hash ||
            candidate.cancelRequested->load(std::memory_order_acquire);
        if (stale) candidate.requestCancel();
        if (!candidate.ready()) return true;

        if (stale) {
            for (auto& task : candidate.tasks) {
                if (!task.valid()) continue;
                try { static_cast<void>(task.get()); } catch (...) {}
            }
            m_candidate.reset();
        } else {
            container::Vector<std::optional<TerrainBibMeshCpu>> cpuProducts(
                candidate.tasks.size());
            bool failed = false;
            for (size_t index = 0; index < candidate.tasks.size(); ++index) {
                try {
                    cpuProducts[index] = candidate.tasks[index].get();
                } catch (const std::exception& exception) {
                    if (!failed) {
                        setError(error, "terrain-bib-cpu task failed: " +
                            container::String(exception.what()));
                    }
                    failed = true;
                } catch (...) {
                    if (!failed) {
                        setError(error, "terrain-bib-cpu task failed");
                    }
                    failed = true;
                }
            }
            container::Vector<TerrainBibRenderData> sources =
                std::move(candidate.sources);
            m_candidate.reset();
            if (failed) return false;

            container::Vector<TerrainGpuBibChunk> next;
            next.reserve(sources.size());
            for (size_t index = 0; index < sources.size(); ++index) {
                const TerrainBibRenderData& source = sources[index];
                if (!cpuProducts[index]) continue;
                TerrainBibMeshCpu& cpu = *cpuProducts[index];
                TerrainGpuBibChunk bib;
                math::vec3 minimum = source.corners.front();
                math::vec3 maximum = minimum;
                for (const RenderVector& corner : source.corners) {
                    minimum = {
                        std::min(minimum.x(), corner.x()),
                        std::min(minimum.y(), corner.y()),
                        std::min(minimum.z(), corner.z()),
                    };
                    maximum = {
                        std::max(maximum.x(), corner.x()),
                        std::max(maximum.y(), corner.y()),
                        std::max(maximum.z(), corner.z()),
                    };
                }
                bib.boundsCenter = (minimum + maximum) * 0.5f;
                bib.boundsRadius = (maximum - bib.boundsCenter).length();
                bib.receivesVisibility = source.receivesVisibility;
                if (!materials.acquireBib(
                        textureName(source), source.tint,
                        bib.geometry.materialIndex,
                        error) ||
                    !geometry.upload(
                        cpu.vertices, cpu.indices, bib.geometry, error)) {
                    geometry.retire(next);
                    return false;
                }
                next.push_back(std::move(bib));
            }
            geometry.retire(m_chunks);
            m_chunks = std::move(next);
            m_contentHash = hash;
            return true;
        }
    }

    if (hash == m_contentHash) return true;
    if (bibs.empty()) {
        geometry.retire(m_chunks);
        m_contentHash = hash;
        return true;
    }

    CpuCandidate candidate;
    candidate.contentHash = hash;
    candidate.sources.assign(bibs.begin(), bibs.end());
    candidate.tasks.reserve(candidate.sources.size());
    const auto cancel = candidate.cancelRequested;
    try {
        for (size_t index = 0; index < candidate.sources.size(); ++index) {
            const TerrainBibRenderData source = candidate.sources[index];
            candidate.tasks.push_back(submitTerrainSceneFuture<
                std::optional<TerrainBibMeshCpu>>(
                    "terrain-bib-cpu", index, 256ull * 1024ull,
                    [source, cancel]() -> std::optional<TerrainBibMeshCpu> {
                        if (cancel->load(std::memory_order_acquire)) {
                            return std::nullopt;
                        }
                        TerrainBibMeshCpu cpu;
                        if (!buildTerrainBibMesh(source, cpu) ||
                            cancel->load(std::memory_order_acquire)) {
                            return std::nullopt;
                        }
                        return cpu;
                    }));
        }
    } catch (const std::exception& exception) {
        candidate.requestCancel();
        m_candidate = std::move(candidate);
        setError(error, "Could not submit terrain-bib-cpu task: " +
            container::String(exception.what()));
        return false;
    } catch (...) {
        candidate.requestCancel();
        m_candidate = std::move(candidate);
        setError(error, "Could not submit terrain-bib-cpu task");
        return false;
    }
    m_candidate = std::move(candidate);
    return true;
}

bool TerrainBibUpdateOwner::ready() const noexcept {
    return !m_candidate && m_requestedContentHash == m_contentHash;
}

void TerrainBibUpdateOwner::requestCancel() noexcept {
    if (m_candidate) m_candidate->requestCancel();
}

void TerrainBibUpdateOwner::retire(
    TerrainGpuGeometryOwner& geometry) noexcept {
    requestCancel();
    geometry.retire(m_chunks);
    m_candidate.reset();
}

const container::Vector<TerrainGpuBibChunk>&
TerrainBibUpdateOwner::chunks() const noexcept {
    return m_chunks;
}

size_t TerrainBibUpdateOwner::chunkCount() const noexcept {
    return m_chunks.size();
}

} // namespace engine::render::detail
