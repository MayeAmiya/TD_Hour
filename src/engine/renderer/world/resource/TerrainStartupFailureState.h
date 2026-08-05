#pragma once

#include <cstdint>

namespace engine::render {

struct PreparedWorldFrame;
struct TerrainRenderSnapshot;

class TerrainStartupFailureState final {
public:
    void clear() noexcept;
    void record(const PreparedWorldFrame& frame) noexcept;

    [[nodiscard]] bool active() const noexcept {
        return m_presentationEpoch != 0;
    }
    [[nodiscard]] bool matches(
        const PreparedWorldFrame& frame) const noexcept;
    [[nodiscard]] bool terrainMatches(
        const TerrainRenderSnapshot* terrain) const noexcept;

private:
    uint64_t m_presentationEpoch = 0;
    uint64_t m_sessionRevision = 0;
    uint64_t m_loadingRevision = 0;
    uint64_t m_terrainRevision = 0;
    uint64_t m_layoutRevision = 0;
    uint64_t m_borderRevision = 0;
    uint64_t m_waterRevision = 0;
    uint64_t m_bridgeRevision = 0;
};

} // namespace engine::render
