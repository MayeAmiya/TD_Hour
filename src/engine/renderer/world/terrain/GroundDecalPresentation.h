#pragma once

#include "core/container/hash_containers.h"
#include "presentation/render/GroundDecalPresentationContracts.h"
#include "engine/renderer/world/terrain/GroundProjectorRenderer.h"

#include <cstdint>

namespace engine::render {

struct GroundDecalPresentationStats final {
    uint32_t activeOwners = 0;
    uint32_t emittedProjectors = 0;
    uint64_t appliedEvents = 0;
    uint64_t staleRejectedEvents = 0;
    uint64_t orphanUpdateEvents = 0;
    uint64_t budgetRejectedOwners = 0;
    uint64_t epochResets = 0;
    uint32_t highWaterOwners = 0;
};

class GroundDecalPresentation final {
public:
    [[nodiscard]] bool submit(const GroundDecalPresentationBatch& batch);
    [[nodiscard]] container::Vector<GroundProjectorInstance> buildProjectors(
        const TerrainRenderSnapshot* terrain,
        const RenderCameraSnapshot* camera = nullptr,
        float viewportAspectRatio = 4.0f / 3.0f) noexcept;
    // Clears output while retaining its allocation, then fills it with this
    // presentation owner's current projectors. The value-return overload is
    // retained for probes and infrequent callers.
    void buildProjectorsInto(
        container::Vector<GroundProjectorInstance>& output,
        const TerrainRenderSnapshot* terrain,
        const RenderCameraSnapshot* camera = nullptr,
        float viewportAspectRatio = 4.0f / 3.0f) noexcept;
    // Append form for a shared frame aggregate. emittedProjectors records
    // only the projectors contributed by this presentation owner.
    void appendProjectors(
        container::Vector<GroundProjectorInstance>& output,
        const TerrainRenderSnapshot* terrain,
        const RenderCameraSnapshot* camera = nullptr,
        float viewportAspectRatio = 4.0f / 3.0f) noexcept;
    void reset(uint64_t presentationEpoch = 0) noexcept;

    [[nodiscard]] uint64_t presentationEpoch() const noexcept {
        return m_presentationEpoch;
    }
    [[nodiscard]] size_t activeOwnerCount() const noexcept {
        return m_active.size();
    }
    [[nodiscard]] const GroundDecalPresentationStats& stats() const noexcept {
        return m_stats;
    }

private:
    struct ActiveRecord final {
        GroundDecalPresentationEvent value;
        uint64_t lastConfirmedFrame = 0;
        uint64_t lastStreamSequence = 0;
        uint64_t lastFadeFrame = 0;
        float fadeOpacity = 1.0f;
        bool fadingOut = false;
    };

    uint64_t m_presentationEpoch = 0;
    uint64_t m_confirmedFrame = 0;
    uint8_t m_observerPlayer = 0xff;
    bool m_drawIconUiEnabled = true;
    container::HashMap<GroundDecalPresentationKey, ActiveRecord,
                       GroundDecalPresentationKeyHash> m_active;
    GroundDecalPresentationStats m_stats;
};

} // namespace engine::render
