#pragma once

#include "core/container/container_types.h"
#include "engine/renderer/world/model/W3dAssetCache.h"

#include <cstdint>

namespace engine::render {

struct PreparedWorldFrame;
struct StartupSceneTicketRenderStats;
class W3dAnimationCache;

class WorldStartupSceneTicket final {
public:
    void begin(uint64_t presentationEpoch, uint64_t sessionRevision,
               uint64_t loadingRevision);
    void reset();
    void addModel(W3dModelHandle handle);
    void addAnimation(container::StringView animation);

    [[nodiscard]] bool matches(
        const PreparedWorldFrame& frame) const noexcept;
    [[nodiscard]] StartupSceneTicketRenderStats projectStats(
        const PreparedWorldFrame& frame,
        const W3dAssetCache& assets,
        const W3dAnimationCache& animations,
        bool terrainReady,
        bool bibRequired,
        bool bibsReady,
        bool requiredFailed) const;

private:
    uint64_t m_presentationEpoch = 0;
    uint64_t m_sessionRevision = 0;
    uint64_t m_loadingRevision = 0;
    container::Vector<W3dModelHandle> m_models;
    container::Vector<container::String> m_animations;
};

} // namespace engine::render
