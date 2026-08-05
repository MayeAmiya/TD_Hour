#include "ScriptViewCompatibilityControls.h"

#include <cmath>

namespace engine::script {

void ScriptViewCompatibilityState::reset(uint64_t presentationEpoch) noexcept {
    m_terrainOversizeTiles = 0;
    m_guardBandX = 0.0f;
    m_guardBandY = 0.0f;
    m_terrainOversizeStamp = {.presentationEpoch = presentationEpoch};
    m_guardBandStamp = {.presentationEpoch = presentationEpoch};
    m_lastMutation = {.presentationEpoch = presentationEpoch};
}

void ScriptViewCompatibilityState::rebindPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    m_terrainOversizeStamp.presentationEpoch = presentationEpoch;
    m_guardBandStamp.presentationEpoch = presentationEpoch;
    m_lastMutation.presentationEpoch = presentationEpoch;
}

bool ScriptViewCompatibilityState::setTerrainOversizeTiles(
    int32_t tiles, ScriptPresentationControlStamp stamp) noexcept {
    if (stamp.presentationEpoch == 0 || stamp.sequence == 0) return false;
    m_terrainOversizeTiles = tiles;
    m_terrainOversizeStamp = stamp;
    m_lastMutation = stamp;
    return true;
}

bool ScriptViewCompatibilityState::setGuardBandBias(
    float x, float y, ScriptPresentationControlStamp stamp) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || stamp.presentationEpoch == 0 ||
        stamp.sequence == 0) {
        return false;
    }
    m_guardBandX = x;
    m_guardBandY = y;
    m_guardBandStamp = stamp;
    m_lastMutation = stamp;
    return true;
}

} // namespace engine::script
