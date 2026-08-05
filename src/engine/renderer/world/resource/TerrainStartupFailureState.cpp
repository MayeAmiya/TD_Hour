#include "engine/renderer/world/resource/TerrainStartupFailureState.h"

#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

namespace engine::render {

void TerrainStartupFailureState::clear() noexcept { *this = {}; }

void TerrainStartupFailureState::record(
    const PreparedWorldFrame& frame) noexcept {
    if (frame.presentationEpoch == 0 || frame.sessionRevision == 0 ||
        frame.loadingRevision == 0) {
        return;
    }
    m_presentationEpoch = frame.presentationEpoch;
    m_sessionRevision = frame.sessionRevision;
    m_loadingRevision = frame.loadingRevision;
    if (!frame.terrain) return;
    m_terrainRevision = frame.terrain->revision;
    m_layoutRevision = frame.terrain->layoutRevision;
    m_borderRevision = frame.terrain->borderShroudRevision;
    m_waterRevision = frame.terrain->waterRevision;
    m_bridgeRevision = frame.terrain->bridgeRevision;
}

bool TerrainStartupFailureState::terrainMatches(
    const TerrainRenderSnapshot* terrain) const noexcept {
    if (!terrain) return m_terrainRevision == 0;
    return m_terrainRevision == terrain->revision &&
        m_layoutRevision == terrain->layoutRevision &&
        m_borderRevision == terrain->borderShroudRevision &&
        m_waterRevision == terrain->waterRevision &&
        m_bridgeRevision == terrain->bridgeRevision;
}

bool TerrainStartupFailureState::matches(
    const PreparedWorldFrame& frame) const noexcept {
    return active() &&
        m_presentationEpoch == frame.presentationEpoch &&
        m_sessionRevision == frame.sessionRevision &&
        m_loadingRevision == frame.loadingRevision &&
        terrainMatches(frame.terrain.get());
}

} // namespace engine::render
