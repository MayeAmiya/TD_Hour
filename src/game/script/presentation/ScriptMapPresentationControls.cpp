#include "ScriptMapPresentationControls.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::script {

void ScriptMapPresentationState::reset(uint64_t presentationEpoch) noexcept {
    m_radarHidden = false;
    m_radarForced = false;
    m_borderShroudEnabled = true;
    m_borderShroudStamp = {.presentationEpoch = presentationEpoch};
    m_boundaryIndex = 0;
    m_radarEvents.clear();
    m_lastMutation = {.presentationEpoch = presentationEpoch};
}

void ScriptMapPresentationState::rebindPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    m_borderShroudStamp.presentationEpoch = presentationEpoch;
    m_lastMutation.presentationEpoch = presentationEpoch;
    for (ScriptRadarEventPresentation& event : m_radarEvents) {
        event.stamp.presentationEpoch = presentationEpoch;
    }
}

bool ScriptMapPresentationState::setRadarHidden(bool hidden,
                                                 ScriptPresentationControlStamp stamp) noexcept {
    if (m_radarHidden == hidden) return false;
    m_radarHidden = hidden;
    m_lastMutation = stamp;
    return true;
}

bool ScriptMapPresentationState::setRadarForced(bool forced,
                                                 ScriptPresentationControlStamp stamp) noexcept {
    if (m_radarForced == forced) return false;
    m_radarForced = forced;
    m_lastMutation = stamp;
    return true;
}

bool ScriptMapPresentationState::setBorderShroudEnabled(
    bool enabled, ScriptPresentationControlStamp stamp) noexcept {
    if (m_borderShroudEnabled == enabled) return false;
    m_borderShroudEnabled = enabled;
    m_borderShroudStamp = stamp;
    m_lastMutation = stamp;
    return true;
}

bool ScriptMapPresentationState::setBoundary(int32_t boundaryIndex,
                                              ScriptPresentationControlStamp stamp) noexcept {
    if (m_boundaryIndex == boundaryIndex) return false;
    m_boundaryIndex = boundaryIndex;
    m_lastMutation = stamp;
    return true;
}

void ScriptMapPresentationState::appendRadarEvent(ScriptRadarEventPresentation event) {
    if (!std::isfinite(event.position.x()) || !std::isfinite(event.position.y()) ||
        !std::isfinite(event.position.z())) {
        return;
    }
    if (event.fadeTick > event.dieTick) event.fadeTick = event.dieTick;
    if (m_radarEvents.size() >= kMaximumRadarEvents) {
        m_radarEvents.erase(m_radarEvents.begin());
    }
    m_lastMutation = event.stamp;
    m_radarEvents.push_back(std::move(event));
}

void ScriptMapPresentationState::noteMapMutation(ScriptPresentationControlStamp stamp) noexcept {
    m_lastMutation = stamp;
}

bool ScriptMapPresentationState::advanceRadarEvents(uint64_t confirmedTick) noexcept {
    const auto expired = std::remove_if(
        m_radarEvents.begin(), m_radarEvents.end(), [confirmedTick](const auto& event) {
            return confirmedTick > event.dieTick;
        });
    if (expired == m_radarEvents.end()) return false;
    m_radarEvents.erase(expired, m_radarEvents.end());
    return true;
}

} // namespace engine::script
