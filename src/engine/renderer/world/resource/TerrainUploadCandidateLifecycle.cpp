#include "engine/renderer/world/resource/TerrainUploadCandidateLifecycle.h"

namespace engine::render {

TerrainUploadCandidateLifecycle::~TerrainUploadCandidateLifecycle() {
    cancelAll();
}

void TerrainUploadCandidateLifecycle::cancelAll() {
    if (m_current) m_current->requestCancel();
    for (auto& candidate : m_retired) candidate->requestCancel();
    m_current.reset();
    m_retired.clear();
}

void TerrainUploadCandidateLifecycle::retireCurrent() {
    if (!m_current) return;
    m_current->requestCancel();
    m_retired.push_back(std::move(m_current));
}

void TerrainUploadCandidateLifecycle::reapRetired() {
    for (auto candidate = m_retired.begin(); candidate != m_retired.end();) {
        if ((*candidate)->readyToDestroy()) {
            candidate = m_retired.erase(candidate);
        } else {
            ++candidate;
        }
    }
}

void TerrainUploadCandidateLifecycle::adopt(
    container::UniquePtr<Candidate> candidate) noexcept {
    m_current = std::move(candidate);
}

void TerrainUploadCandidateLifecycle::completeCurrent() noexcept {
    m_current.reset();
    m_fallbackActive = false;
    m_textureUpgradePending = false;
}

void TerrainUploadCandidateLifecycle::resetState() {
    cancelAll();
    m_fallbackActive = false;
    m_textureUpgradePending = false;
}

void TerrainUploadCandidateLifecycle::invalidateForTexturePolicy() {
    retireCurrent();
    m_fallbackActive = false;
    m_textureUpgradePending = false;
}

void TerrainUploadCandidateLifecycle::failCurrent() {
    retireCurrent();
    m_textureUpgradePending = false;
}

} // namespace engine::render
