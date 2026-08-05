#pragma once

#include "engine/renderer/world/resource/RenderAssetScheduling.h"
#include "engine/renderer/world/resource/WorldTextureContracts.h"
#include "engine/texture/TextureManager.h"

#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>
#include <optional>

namespace engine::render {

// CPU-side texture source/decode owner. It owns scheduler tickets, decoder
// leases, source aliases, negative lookups and prepared immutable variants;
// no D3D12 resource or cache residency state crosses this boundary.
class WorldTextureDecodeService final {
public:
    struct Diagnostics final {
        size_t queuedJobs = 0;
        size_t activeJobs = 0;
        size_t pendingSources = 0;
        size_t activeSourceJobs = 0;
        size_t pendingVariants = 0;
        size_t activeVariantJobs = 0;
        size_t preparedVariants = 0;
        size_t failedVariants = 0;
        uint64_t staleCompletions = 0;
        uint64_t completedCpuJobs = 0;
        uint64_t preparedBytes = 0;
        uint64_t workerNanoseconds = 0;
        uint64_t cancelledVariants = 0;
        uint64_t cancelledReady = 0;
        uint64_t cancelRequestedActive = 0;
        uint32_t maximumQueueAge = 0;
        uint64_t retainedPreparedBytes = 0;
        uint64_t reclaimedPreparedBytes = 0;
        uint64_t reclaimedSourceBytes = 0;
        uint64_t reclaimedSources = 0;
    };

    enum class State : uint8_t {
        Pending,
        Ready,
        Failed,
    };

    enum class Phase : uint8_t {
        Queued,
        Active,
        Ready,
        Failed,
    };

    struct Lookup final {
        State state = State::Pending;

        struct Payload final {
            struct Mip final {
                uint32_t byteOffset = 0;
                uint32_t rowPitch = 0;
                uint32_t slicePitch = 0;
            };

            container::SharedPtr<const container::Vector<uint8_t>> pixels;
            container::Vector<Mip> mips;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t sourceWidth = 0;
            uint32_t sourceHeight = 0;
            uint64_t byteSize = 0;
        };

        container::SharedPtr<const Payload> payload;
        container::String diagnostic;
    };

    WorldTextureDecodeService();
    ~WorldTextureDecodeService();

    WorldTextureDecodeService(const WorldTextureDecodeService&) = delete;
    WorldTextureDecodeService& operator=(const WorldTextureDecodeService&) = delete;

    [[nodiscard]] Lookup requestOrdinary(
        container::StringView logicalName,
        container::String sourceKey,
        container::String variantKey,
        WorldTextureVariant variant,
        uint32_t reduction,
        RenderAssetPriority priority);
    [[nodiscard]] Lookup requestTerrainColor(
        container::StringView logicalName,
        container::String sourceKey,
        container::String variantKey,
        uint32_t gridWidth,
        uint32_t reduction,
        RenderAssetPriority priority);
    [[nodiscard]] Lookup requestTerrainAlphaEdge(
        container::StringView logicalName,
        container::String sourceKey,
        container::String variantKey,
        RenderAssetPriority priority);

    void pumpCompletions();
    void discardPreparedVariant(container::StringView variantKey);
    size_t collectSourceGarbage(uint64_t maximumBytes, size_t maximumItems);
    void reset();
    void invalidateVariants();

    [[nodiscard]] TextureManagerStats stats() const noexcept;
    [[nodiscard]] Diagnostics diagnostics() const noexcept;
    [[nodiscard]] std::optional<Phase> phase(
        container::StringView variantKey) const noexcept;

private:
    class Impl;
    container::UniquePtr<Impl> m_impl;
};

} // namespace engine::render
